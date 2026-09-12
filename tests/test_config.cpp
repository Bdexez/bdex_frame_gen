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
    CHECK(c.presentMode == -1);
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
