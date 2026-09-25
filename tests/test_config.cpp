#include "config.h"
#include "platform.h"
#include "test.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using bdex::Config;

TEST(config_defaults) {
    Config c;
    CHECK(!c.enabled);   // loaded-but-inert until a config/env turns it on
    CHECK(c.multiplier == 2);
    CHECK(c.presentMode == -2);  // auto
    CHECK(c.debug == Config::Debug::None);
}

TEST(config_apply_values) {
    Config c;
    CHECK(c.apply("multiplier", "3") && c.multiplier == 3);
    CHECK(c.apply("MULTIPLIER", " 4 ") && c.multiplier == 4);
    CHECK(c.apply("multiplier", "9") && c.multiplier == 4);   // clamped
    CHECK(c.apply("enabled", "false") && !c.enabled);
    CHECK(c.apply("enabled", "on") && c.enabled);
    CHECK(c.apply("present_mode", "mailbox") && c.presentMode == VK_PRESENT_MODE_MAILBOX_KHR);
    CHECK(c.apply("present_mode", "app") && c.presentMode == -1);
    CHECK(c.apply("present_mode", "auto") && c.presentMode == -2);
    CHECK(c.apply("mode", "extrapolate") && c.extrapolate);
    CHECK(c.apply("mode", "interpolate") && !c.extrapolate);
    CHECK(!c.apply("mode", "guess"));
    CHECK(c.apply("flow_scale", "4") && c.flowScale == 4);
    CHECK(!c.apply("flow_scale", "3"));
    CHECK(c.apply("flow_scale", "auto") && c.flowScale == 0);
    CHECK(c.flowScaleFor(1920, 1080) == 2 && c.flowScaleFor(3840, 2160) == 4 && c.flowScaleFor(800, 600) == 1);
    CHECK(c.apply("fullres", "1") && c.flowScale == 1);
    CHECK(c.apply("debug", "flow") && c.debug == Config::Debug::Flow);
    CHECK(c.apply("debug", "passthrough") && c.debug == Config::Debug::Passthrough);
    CHECK(c.apply("search", "3") && c.searchRadius == 3);
    CHECK(c.apply("scene_cut_low", "0.5") && c.sceneCutLow == 0.5f);
    CHECK(c.apply("log_file", "/tmp/x.log") && c.logFile == "/tmp/x.log");
    // HUD detail level: numeric, named, or boolean words.
    CHECK(c.apply("hud", "3") && c.overlayMode == 3);
    CHECK(c.apply("overlay", "off") && c.overlayMode == 0);
    CHECK(c.apply("hud", "fps") && c.overlayMode == 1);
    CHECK(c.apply("hud", "full") && c.overlayMode == 4);
    CHECK(c.apply("hud", "on") && c.overlayMode == 2);
    CHECK(c.apply("hud", "9") && c.overlayMode == 5);    // clamped to the top level
    CHECK(c.apply("hud", "latency") && c.overlayMode == 5);
    CHECK(!c.apply("hud", "loud"));                      // unknown word rejected
    CHECK(c.apply("census_weight", "0.5") && c.censusWeight == 0.5f);
    CHECK(c.apply("CENSUS_MARGIN", "0.1") && c.censusMargin == 0.1f);
    CHECK(c.apply("census_weight", "9") && c.censusWeight == 8.f);   // clamped
    CHECK(c.apply("grad_weight", "1") && c.gradWeight == 1.0f);
    CHECK(c.apply("rot_weight", "0.5") && c.rotWeight == 0.5f);
    CHECK(c.apply("ROT_WEIGHT", "1") && c.rotWeight == 1.0f);
    CHECK(c.apply("rot_weight", "9") && c.rotWeight == 1.f);   // clamped
    CHECK(c.apply("occlusion_strength", "0.6") && c.occlusionStrength == 0.6f);
    CHECK(c.apply("occlusion_strength", "2") && c.occlusionStrength == 1.0f);
    CHECK(c.apply("static_guard", "1") && c.staticGuard == 1.0f);
    CHECK(c.apply("TEMPORAL_PREDICTION", "1") && c.temporalPrediction);
    CHECK(c.apply("extrapolate_hold", "1") && c.extrapolateHold == 1.0f);
    CHECK(c.apply("zero_bias_contrast", "0.05") && c.zeroBiasContrast == 0.05f);
    CHECK(c.apply("subpixel", "1") && c.subpixel);
    CHECK(c.apply("pixel_vectors", "1") && c.pixelVectors);
    CHECK(c.apply("fps_limit", "60") && c.fpsLimit == 60.f);
    CHECK(c.apply("target_fps", "144") && c.targetFps == 144.f);
    CHECK(c.apply("paused", "1") && c.paused);
    {
        Config live, fresh;
        fresh.zeroBias = 0.01f; fresh.multiplier = 4; fresh.paused = true; fresh.renderScale = 0.5f;
        live.applyLive(fresh);
        CHECK(live.zeroBias == 0.01f && live.paused);           // live settings follow
        CHECK(live.multiplier == 2 && live.renderScale == 1.f); // resource-sizing ones do not
    }
    CHECK(c.apply("FPS_LIMIT", "5000") && c.fpsLimit == 1000.f);   // clamped
    CHECK(c.apply("extrapolate_splat", "1") && c.extrapolateSplat);
    CHECK(c.apply("STATIC_GUARD", "3") && c.staticGuard == 1.0f);   // clamped
    CHECK(!c.frameTiming && !c.dedup);   // opt-in
    CHECK(c.apply("frame_timing", "1") && c.frameTiming);
    CHECK(c.apply("DEDUP", "on") && c.dedup);
    CHECK(!c.sceneCutLocal && c.apply("scene_cut_local", "1") && c.sceneCutLocal);
    CHECK(c.apply("extrapolate_accel", "0.5") && c.extrapolateAccel == 0.5f);
    CHECK(c.apply("flow_temporal", "1") && c.flowTemporal == 1.f);
    CHECK(c.apply("flow_temporal", "2") && c.flowTemporal == 1.f);  // clamped
    CHECK(c.apply("online_tune", "1") && c.onlineTune);
    CHECK(c.describe().find(" online_tune=1") != std::string::npos);
    CHECK(c.apply("chroma_weight", "1") && c.chromaWeight == 1.f);
    CHECK(c.apply("gpu_budget", "0.3") && c.gpuBudget == 0.3f);
    CHECK(c.apply("target_fps", "auto") && c.targetFps < 0.f);
    CHECK(c.apply("FPS_LIMIT", "Auto") && c.fpsLimit < 0.f);
    CHECK(c.apply("fps_limit", "72") && c.fpsLimit == 72.f);
    CHECK(c.presentFeedback && c.apply("present_feedback", "0") && !c.presentFeedback);
    CHECK(c.captureHdr == -1 && c.apply("capture_hdr", "1") && c.captureHdr == 1);
    CHECK(c.apply("CAPTURE_HDR", "auto") && c.captureHdr == -1 && c.apply("capture_hdr", "off") && c.captureHdr == 0);
    CHECK(c.apply("EXTRAPOLATE_ACCEL", "3") && c.extrapolateAccel == 1.f);   // clamped
    {
        Config live, fresh;
        fresh.frameTiming = true; fresh.dedup = true;
        live.applyLive(fresh);
        CHECK(live.frameTiming && !live.dedup);   // dedup sizes the output stage
    }
}

TEST(config_rejects_garbage) {
    Config c;
    CHECK(!c.apply("multiplier", "two"));
    CHECK(!c.apply("enabled", "maybe"));
    CHECK(!c.apply("present_mode", "turbo"));
    CHECK(!c.apply("no_such_key", "1"));
    CHECK(c.multiplier == 2 && !c.enabled);
}

TEST(config_bundled_profiles_are_game_only) {
    const std::string path = test::tmpPath("bdex_test_profiles.conf");
    {
        std::ofstream f(path);
        f << "multiplier = 4\n"            // before any section: ignored when bundled
          << "[*]\nmultiplier = 4\nenabled = 1\n"  // global: ignored when bundled
          << "[mygame]\nenabled = 1\npaused = 1\nmultiplier = 3\nmode = interpolate\n";
    }
    Config c;
    c.loadFile(path, "/games/mygame.exe", true);
    CHECK(c.multiplier == 3);            // the game's own setting applies
    CHECK(!c.enabled && !c.paused);      // but a profile never turns the layer on or pauses it
    Config other;
    other.loadFile(path, "/games/othergame.exe", true);
    CHECK(other.multiplier == 2);        // neither the loose line nor [*] applied
    remove(path.c_str());
}

TEST(config_file_and_env_priority) {
    const std::string path = test::tmpPath("bdex_test_config.conf");
    {
        std::ofstream f(path);
        f << "# comment\nmultiplier = 3\nlevels=5\n\nbogus line\nsearch = 1\n";
    }
    setenv("BDEX_FG_CONFIG", path.c_str(), 1);
    setenv("BDEX_FG_LEVELS", "2", 1);
    unsetenv("BDEX_FG_MULTIPLIER");
    Config c = Config::load();
    CHECK(c.multiplier == 3);   // from file
    CHECK(c.levels == 2);       // env overrides file
    CHECK(c.searchRadius == 1);
    unsetenv("BDEX_FG_CONFIG");
    unsetenv("BDEX_FG_LEVELS");
    remove(path.c_str());
}

TEST(config_product_defaults_and_mode_priority) {
    const std::string path = test::tmpPath("bdex_test_capture_defaults.conf");
    { std::ofstream f(path); f << "mode=interpolate\n"; }
    setenv("BDEX_FG_CONFIG", path.c_str(), 1);
    unsetenv("BDEX_FG_MODE");
    Config defaults;
    defaults.extrapolate = true;
    CHECK(!Config::loadWithDefaults(defaults).extrapolate); // file overrides capture default
    setenv("BDEX_FG_MODE", "extrapolate", 1);
    CHECK(Config::loadWithDefaults(defaults).extrapolate);  // env overrides file
    unsetenv("BDEX_FG_MODE");
    { std::ofstream f(path); f << ""; }
    CHECK(Config::loadWithDefaults(defaults).extrapolate);  // capture default survives
    CHECK(!Config::load().extrapolate);                     // layer default is unchanged
    unsetenv("BDEX_FG_CONFIG");
    remove(path.c_str());
}

TEST(config_master_switch) {
    setenv("BDEX_FG", "0", 1);
    Config c = Config::load();
    CHECK(!c.enabled);
    setenv("BDEX_FG", "1", 1);
    c = Config::load();
    CHECK(c.enabled);
    unsetenv("BDEX_FG");
}

TEST(config_scene_cut_ordering) {
    setenv("BDEX_FG_SCENE_CUT_LOW", "0.9", 1);
    setenv("BDEX_FG_SCENE_CUT_HIGH", "0.1", 1);
    Config c = Config::load();
    CHECK(c.sceneCutHigh >= c.sceneCutLow);
    unsetenv("BDEX_FG_SCENE_CUT_LOW");
    unsetenv("BDEX_FG_SCENE_CUT_HIGH");
}

TEST(config_sections) {
    const std::string path = test::tmpPath("bdex_test_sections.conf");
    {
        std::ofstream f(path);
        f << "multiplier = 2\n[MyGame.exe]\nmultiplier = 3\n[*]\nlevels = 5\n[other]\nlevels = 1\n";
    }
    Config c;
    c.loadFile(path, "/games/bin/mygame.exe");
    CHECK(c.multiplier == 3);   // section matched the process name
    CHECK(c.levels == 5);       // [*] applies everywhere
    Config d;
    d.loadFile(path, "C:\\Games\\Other.EXE");
    CHECK(d.multiplier == 2);
    CHECK(d.levels == 1);
    remove(path.c_str());
}

TEST(config_enable_from_file) {
    // The layer is always loaded now; a config file alone (no BDEX_FG env) must
    // be able to turn it on, globally via [*] or for one game via its section.
    const std::string path = test::tmpPath("bdex_test_enable.conf");
    {
        std::ofstream f(path);
        f << "[*]\nenabled = 0\n[kenshi]\nenabled = 1\nmultiplier = 3\n";
    }
    Config on;
    on.loadFile(path, "/games/Kenshi/kenshi.exe");
    CHECK(on.enabled && on.multiplier == 3);   // game section opts in
    Config off;
    off.loadFile(path, "/usr/bin/firefox");
    CHECK(!off.enabled);                        // other apps stay pass-through
    {
        std::ofstream f(path);
        f << "[*]\nenabled = 1\n";              // global master switch on
    }
    Config all;
    all.loadFile(path, "/usr/bin/anything");
    CHECK(all.enabled);
    remove(path.c_str());
}

TEST(config_section_matching) {
    std::vector<std::string> names = {"game", "wine64-preloader"};
    CHECK(Config::sectionMatches("Game.exe", names));
    CHECK(Config::sectionMatches("game", names));
    CHECK(Config::sectionMatches("*", names));
    CHECK(Config::sectionMatches("", names));
    CHECK(!Config::sectionMatches("othergame", names));
    CHECK(!Config::processNames().empty());  // at least our own executable
}

TEST(config_presets) {
    Config c;
    CHECK(c.apply("preset", "performance") && c.flowScale == 4 && !c.refineAll);
    CHECK(c.apply("preset", "quality") && c.flowScale == 1 && c.flowIterations == 2);
    CHECK(!c.apply("preset", "ultra"));
    // A preset in the environment is applied before the individual variables.
    setenv("BDEX_FG_FLOW_SCALE", "2", 1);
    setenv("BDEX_FG_PRESET", "performance", 1);
    Config d = Config::load();
    CHECK(d.flowScale == 2 && !d.refineAll);
    unsetenv("BDEX_FG_FLOW_SCALE");
    unsetenv("BDEX_FG_PRESET");
}

// log_file may contain an environment reference in the platform's own syntax
// (the Windows docs recommend `log_file=%TEMP%\bdex-fg.log`). Without
// expansion the layer opens a file named literally "%TEMP%\..." and silently
// logs nowhere, which would break the very first Windows run.
TEST(config_expand_log_file) {
    using bdex::platformExpandPath;
    CHECK(platformExpandPath("") == "");
    CHECK(platformExpandPath("plain/relative.log") == "plain/relative.log");
#ifdef _WIN32
    setenv("BDEX_FG_TESTDIR", "C:\\tmp", 1);
    CHECK(platformExpandPath("%BDEX_FG_TESTDIR%\\bdex-fg.log") == "C:\\tmp\\bdex-fg.log");
    unsetenv("BDEX_FG_TESTDIR");
    // An undefined variable is left verbatim rather than emptied.
    CHECK(platformExpandPath("%BDEX_FG_NOPE%\\x") == "%BDEX_FG_NOPE%\\x");
#else
    setenv("BDEX_FG_TESTDIR", "/var/tmp", 1);
    CHECK(platformExpandPath("$BDEX_FG_TESTDIR/bdex-fg.log") == "/var/tmp/bdex-fg.log");
    CHECK(platformExpandPath("${BDEX_FG_TESTDIR}/bdex-fg.log") == "/var/tmp/bdex-fg.log");
    unsetenv("BDEX_FG_TESTDIR");
    // An unknown variable is left as-is.
    CHECK(platformExpandPath("$BDEX_FG_NOPE/x") == "$BDEX_FG_NOPE/x");
    // A leading ~ expands to $HOME.
    if (const char* h = std::getenv("HOME"))
        CHECK(platformExpandPath("~/bdex-fg.log") == std::string(h) + "/bdex-fg.log");
#endif
}

TEST(config_tune_name) {
    // The online_tune cache key: the game a capture runs for, else this process.
    CHECK(Config::load("C:\\Games\\Skyrim.exe").tuneName == "skyrim");
    CHECK(!Config::load().tuneName.empty());
}

TEST(config_option_registry) {
    const auto keys = Config::optionKeys();
    auto has = [&](const char* k) { return std::find(keys.begin(), keys.end(), k) != keys.end(); };
    CHECK(has("flow_temporal") && has("sharpen") && has("present_mode") && has("mode"));
    CHECK(!has("no_such_option"));
    Config c;
    CHECK(!c.apply("no_such_option", "1"));
    // Candidates in trial order: flow options first, then the synthesis
    // options of this mode; sub-pixel only in extrapolation.
    auto cand = c.tuneCandidates();
    CHECK(cand.size() == 8 && cand[0].key == "zero_bias_contrast" && cand[0].value == "0.1" && !cand[0].synthesis &&
          cand[2].key == "flow_temporal" && cand[3].key == "occlusion_strength" && cand[3].synthesis &&
          cand[5].key == "synth_bicubic" && cand[6].key == "subpixel_lk+synth_bicubic" && cand[6].value == "2+1" &&
          cand[6].synthesis && cand[7].key == "subpixel+synth_bicubic");
    c.extrapolate = true;
    cand = c.tuneCandidates();
    CHECK(cand.size() == 8 && cand[2].key == "subpixel" && cand[4].key == "subpixel_lk" &&
          cand[5].key == "extrapolate_splat" && cand[5].synthesis && cand[7].key == "synth_bicubic");
}

TEST(config_apply_live_follows_the_registry) {
    Config c, n;
    n.zeroBiasContrast = 0.2f;  // live
    n.frameTiming = true;       // live
    n.levels = 6;               // sizes resources: not live
    n.dedup = true;             // not live
    n.fpsLimit = 50.f;          // live (custom syntax, copied by hand)
    c.applyLive(n);
    CHECK(c.zeroBiasContrast == 0.2f && c.frameTiming && c.fpsLimit == 50.f);
    CHECK(c.levels != 6 && !c.dedup);
}
