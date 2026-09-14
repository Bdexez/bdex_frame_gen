#include "config.h"
#include "test.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using bdex::Config;

TEST(config_defaults) {
    Config c;
    CHECK(c.enabled);
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
}

TEST(config_rejects_garbage) {
    Config c;
    CHECK(!c.apply("multiplier", "two"));
    CHECK(!c.apply("enabled", "maybe"));
    CHECK(!c.apply("present_mode", "turbo"));
    CHECK(!c.apply("no_such_key", "1"));
    CHECK(c.multiplier == 2 && c.enabled);
}

TEST(config_file_and_env_priority) {
    const std::string path = "/tmp/bdex_test_config.conf";
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
    const std::string path = "/tmp/bdex_test_sections.conf";
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
