#include "config.h"
#include "platform.h"
#include "test.h"

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
    CHECK(c.apply("hud", "9") && c.overlayMode == 4);    // clamped to the top level
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
    CHECK(c.apply("FPS_LIMIT", "5000") && c.fpsLimit == 1000.f);   // clamped
    CHECK(c.apply("extrapolate_splat", "1") && c.extrapolateSplat);
    CHECK(c.apply("STATIC_GUARD", "3") && c.staticGuard == 1.0f);   // clamped
}

TEST(config_rejects_garbage) {
    Config c;
    CHECK(!c.apply("multiplier", "two"));
    CHECK(!c.apply("enabled", "maybe"));
    CHECK(!c.apply("present_mode", "turbo"));
    CHECK(!c.apply("no_such_key", "1"));
    CHECK(c.multiplier == 2 && !c.enabled);
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
