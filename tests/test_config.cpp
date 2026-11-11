#include "config.h"
#include "frame_order.h"
#include "platform.h"
#include "test.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using bdex::Config;

TEST(config_defaults) {
    Config c;
    CHECK(!c.enabled);   // loaded-but-inert until a config/env turns it on
    CHECK(c.multiplier == 2);
    CHECK(!c.skipFast && c.refreshFill == 0);  // fixed xN stays xN, even beyond the screen's rate
    CHECK(c.presentMode == -2);  // auto
    CHECK(c.debug == Config::Debug::None);
}

TEST(config_present_capacity_includes_the_real_frame_at_x8) {
    for (int m = 2; m <= 8; ++m) {
        for (int mode = 0; mode < 3; ++mode) {
            for (bool even : {false, true}) {
                Config c;
                c.multiplier = m;
                c.extrapolate = mode == 1;
                c.hybrid = mode == 2;
                c.evenMotion = even;  // inert without refresh_fill
                CHECK(c.presentCeiling() == m);
                CHECK(c.outputCeiling() + 1 == c.presentCeiling());
                // A surface with min=3, max=8 allows six images held at
                // once. Every other output needs transit storage so that
                // submitting the whole job does not wait for its pacing.
                const int acquired = std::min(m, 8 - 3 + 1);
                const int staged = std::max(0, c.presentCeiling() - acquired);
                CHECK(acquired + staged == m);
            }
        }
    }
    Config c;
    c.multiplier = 8;
    c.refreshFill = 1;
    CHECK(c.frameCeiling() == 8 && c.presentCeiling() == 8 && c.outputCeiling() == 7);
    c.multiplier = 7;
    CHECK(c.frameCeiling() == 8 && c.presentCeiling() == 8 && c.outputCeiling() == 7);
    c.evenMotion = true;
    // even_motion presents only generated images: seven output slots;
    // the real frame remains the eighth, unpresented timing anchor.
    CHECK(c.presentCeiling() == 7 && c.outputCeiling() == 7);
}

TEST(config_apply_values) {
    Config c;
    CHECK(c.apply("multiplier", "3") && c.multiplier == 3);
    CHECK(c.apply("MULTIPLIER", " 4 ") && c.multiplier == 4);
    CHECK(c.apply("multiplier", "9") && c.multiplier == 8);   // clamped
    CHECK(c.apply("enabled", "false") && !c.enabled);
    CHECK(c.apply("enabled", "on") && c.enabled);
    // profile: a switch, and 2 for the per-pass timings too.
    CHECK(c.apply("profile", "2") && c.profile && c.profilePasses);
    CHECK(c.apply("profile", "1") && c.profile && !c.profilePasses);
    CHECK(c.apply("profile", " 2 ") && c.apply("profile", "0") && !c.profile && !c.profilePasses);
    CHECK(c.apply("present_mode", "mailbox") && c.presentMode == VK_PRESENT_MODE_MAILBOX_KHR);
    CHECK(c.apply("present_mode", "app") && c.presentMode == -1);
    CHECK(c.apply("present_mode", "auto") && c.presentMode == -2);
    CHECK(c.apply("mode", "extrapolate") && c.extrapolate && !c.hybrid);
    CHECK(c.apply("mode", "hybrid") && c.hybrid && !c.extrapolate && std::string(c.modeName()) == "hybrid");
    CHECK(c.apply("mode", "interpolate") && !c.extrapolate && !c.hybrid);
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
    CHECK(c.apply("hud", "9") && c.overlayMode == 6);    // clamped to the top level
    CHECK(c.apply("overlay", "pacing") && c.overlayMode == 6);
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
    CHECK(c.apply("target_fps", "-1") && c.targetFps < 0.f);  // the UIs' spin buttons
    CHECK(c.apply("fps_limit", "-1") && c.fpsLimit < 0.f);
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

// A game or toolkit may set a locale with a decimal comma (French on the
// SteamOS report): the config keeps its dot.
TEST(config_numbers_ignore_the_locale) {
    const char* old = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = old ? old : "C";
    bool comma = false;
    for (const char* name : {"fr_FR.UTF-8", "fr_FR.utf8", "de_DE.UTF-8", "de_DE.utf8"})
        if (std::setlocale(LC_NUMERIC, name)) { comma = true; break; }
    Config c;
    CHECK(c.apply("scene_cut_low", "0.3") && c.sceneCutLow == 0.3f);
    CHECK(c.apply("scene_cut_high", "+0.6") && c.sceneCutHigh == 0.6f);
    CHECK(!c.apply("scene_cut_low", "0,3"));
    std::setlocale(LC_NUMERIC, saved.c_str());
    if (!comma) std::printf("  (no decimal-comma locale installed: checked in the C locale only)\n");
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

TEST(config_game_mode_without_panel) {
    // The panel's « sans le panneau » (Steam's Game Mode): require_app = 0 in
    // a game's own section turns it on without the panel; [*] alone does not
    // name the process (desktop programs are then excluded, as under
    // require_app), and a shipped profile never can.
    const std::string path = test::tmpPath("bdex_test_game_mode.conf");
    {
        std::ofstream f(path);
        f << "[*]\nenabled = 1\nrequire_app = 1\n[mygame]\nrequire_app = 0\n";
    }
    Config game;
    game.loadFile(path, "/games/mygame.exe");
    CHECK(game.enabled && !game.requireApp && game.processNamed);
    Config other;
    other.loadFile(path, "/games/othergame.exe");
    CHECK(other.enabled && other.requireApp && !other.processNamed);
    Config bundled;
    bundled.loadFile(path, "/games/mygame.exe", true);
    CHECK(!bundled.processNamed);
    remove(path.c_str());
    // BDEX_FG=1 (a Steam launch option) names the process too.
    setenv("BDEX_FG", "1", 1);
    Config env = Config::load();
    CHECK(env.enabled && !env.requireApp && env.processNamed);
    unsetenv("BDEX_FG");
    // Game Mode's compositor and Steam's own processes are desktop programs.
    CHECK(bdex::platformIsNonGame("gamescope") && bdex::platformIsNonGame("steamwebhelper") && bdex::platformIsNonGame("mangoapp"));
    CHECK(bdex::platformIsNonGame("fossilize_replay"));  // (Steam's shader pre-caching)
    CHECK(!bdex::platformIsNonGame("mygame"));
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
    CHECK(c.apply("preset", "quality") && c.flowScale == -1 && c.flowIterations == 2);
    // quality: full-resolution flow up to ~1080p, half above (the pyramid's reach at 1440p)
    CHECK(c.flowScaleFor(1920, 1080) == 1 && c.flowScaleFor(2560, 1440) == 2 && c.flowScaleFor(3840, 2160) == 2);
    CHECK(!c.apply("preset", "ultra"));
    // A preset in the environment is applied before the individual variables.
    setenv("BDEX_FG_FLOW_SCALE", "2", 1);
    setenv("BDEX_FG_PRESET", "performance", 1);
    Config d = Config::load();
    CHECK(d.flowScale == 2 && !d.refineAll);
    unsetenv("BDEX_FG_FLOW_SCALE");
    unsetenv("BDEX_FG_PRESET");
}

// Competitive games keep the layer off even when their anti-cheat has no
// process to detect (VAC, Vanguard...). Names arrive normalised.
TEST(config_competitive_games) {
    using bdex::platformIsCompetitiveGame;
    CHECK(platformIsCompetitiveGame("cs2"));
    CHECK(platformIsCompetitiveGame("dota2"));
    CHECK(platformIsCompetitiveGame("r5apex"));
    CHECK(platformIsCompetitiveGame("league of legends"));
    CHECK(!platformIsCompetitiveGame("bdex_demo"));
    CHECK(!platformIsCompetitiveGame("skyrimse"));
    CHECK(!platformIsCompetitiveGame("cs"));  // exact names only
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
    CHECK(cand.size() == 9 && cand[2].key == "subpixel" && cand[4].key == "subpixel_lk" &&
          cand[5].key == "extrapolate_splat" && cand[5].synthesis && cand[7].key == "reveal_fill" &&
          cand[8].key == "synth_bicubic");
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

TEST(config_option_info) {
    bool seenSmooth = false, seenRefine = false;
    for (const auto& i : Config::optionInfo()) {
        if (i.key == "smoothness") {
            seenSmooth = true;
            CHECK(i.kind == "float" && i.def == "0.003" && i.live && i.hi == 1.0);
        }
        if (i.key == "refine") {
            seenRefine = true;
            CHECK(i.kind == "bool" && i.def == "1" && !i.live);
        }
    }
    CHECK(seenSmooth && seenRefine);
}

TEST(config_preset_enhanced) {
    Config c;
    CHECK(c.applyPreset("enhanced"));
    CHECK(!c.autoTune && c.pixelVectors && c.zeroBiasContrast == 0.02f && c.hudMask == 24 && c.dedup &&
          c.extrapolateSplat && c.flowScale == 0);
    // A later explicit option still wins over the preset.
    CHECK(c.apply("pixel_vectors", "0") && !c.pixelVectors);
}

// The order of a present's frames on screen and their synthesis times: the
// pure modes keep their historical values exactly (i/m, 1 + i/m), hybrid
// puts one interpolated frame first, then the real one, then predictions.
TEST(frame_order_positions_and_times) {
    using bdex::FrameOrder;
    bdex::Config c;
    for (int m = 2; m <= 4; ++m) {
        c.extrapolate = false;
        c.hybrid = false;
        const FrameOrder in = FrameOrder::of(c, m);
        CHECK(in.real == m - 1);
        for (int i = 1; i < m; ++i) CHECK(in.t(i - 1) == 0.f + float(i) / float(m) && !in.predicted(i - 1));
        c.extrapolate = true;
        const FrameOrder ex = FrameOrder::of(c, m);
        CHECK(ex.real == 0);
        for (int i = 1; i < m; ++i) CHECK(ex.t(i - 1) == 1.f + float(i) / float(m) && ex.predicted(i - 1));
    }
    c.extrapolate = false;
    c.hybrid = true;
    CHECK(FrameOrder::of(c, 2).real == 1 && FrameOrder::of(c, 2).t(0) == 0.5f);  // x2: interpolation
    const FrameOrder h4 = FrameOrder::of(c, 4);
    CHECK(h4.real == 1);
    CHECK(h4.position(0) == 0 && h4.position(1) == 2 && h4.position(2) == 3);
    CHECK(h4.t(0) == 0.75f && h4.t(1) == 1.25f && h4.t(2) == 1.5f);
    CHECK(!h4.predicted(0) && h4.predicted(1) && h4.predicted(2));
    const FrameOrder h3 = FrameOrder::of(c, 3);
    CHECK(h3.t(0) == 2.f / 3.f && h3.t(1) == 1.f + 1.f / 3.f);
    CHECK(FrameOrder::of(c, 1).real == 0);  // a lone real frame
}

// interp_hold: only where the caller allows it (the layer's present path),
// 1 = the historical order bit for bit; below 1 the positions up to the real
// frame come closer together, and every frame's content time stays a
// constant distance behind its display time (no wrong motion).
TEST(frame_order_interp_hold) {
    using bdex::FrameOrder;
    bdex::Config c;
    c.interpHold = 0.5f;
    CHECK(FrameOrder::of(c, 2).hold == 1.f);         // not asked for: capture app, bdex_offline
    for (int m = 2; m <= 4; ++m) {
        c.interpHold = 1.f;
        const FrameOrder even = FrameOrder::of(c, m, true);
        CHECK(even.hold == 1.f);
        for (int p = 0; p < m; ++p) CHECK(even.offset(p) == double(p) / double(m));
        for (int i = 1; i < m; ++i) CHECK(even.t(i - 1) == float(i) / float(m));
        for (float h : {0.25f, 0.5f, 0.75f}) {
            c.interpHold = h;
            const FrameOrder o = FrameOrder::of(c, m, true);
            CHECK(o.hold == h);
            // The real frame shows h*(m-1)/m of an interval after the first one.
            CHECK(std::abs(o.offset(o.real) - double(h) * (m - 1) / m) < 1e-9);
            const double lag = 1.0 - o.offset(o.real);  // content time - display time, in intervals
            for (int out = 0; out + 1 < m; ++out)
                CHECK(std::abs(double(o.t(out)) - (o.offset(o.position(out)) + lag)) < 1e-6);
        }
    }
    c.interpHold = 0.5f;
    c.hybrid = true;  // x4 hybrid: 1 interpolated, the real one, 2 predicted 1/4 apart as before
    const FrameOrder h4 = FrameOrder::of(c, 4, true);
    CHECK(std::abs(h4.offset(1) - 0.125) < 1e-9 && std::abs(h4.offset(2) - 0.375) < 1e-9);
    CHECK(h4.t(0) == 0.875f && h4.t(1) == 1.25f && h4.t(2) == 1.5f);
    c.hybrid = false;
    c.extrapolate = true;  // nothing before the real frame: no hold to shorten
    CHECK(FrameOrder::of(c, 3, true).hold == 1.f);
    CHECK(c.apply("interp_hold", "0.5") && c.interpHold == 0.5f);
    CHECK(c.apply("interp_hold", "auto") && c.interpHold == -1.f);
    CHECK(c.apply("interp_hold", "-1") && c.interpHold == -1.f);
    c.extrapolate = false;
    CHECK(FrameOrder::of(c, 2, true).hold == 0.1f);  // auto: the smallest, raised by the caller to one refresh
}

TEST(config_debug_confidence_is_described) {
    // (describe() indexed a four-name table with Debug::Confidence: found by tests/fuzz)
    Config c;
    CHECK(c.apply("debug", "confidence"));
    CHECK(c.describe().find("debug=confidence") != std::string::npos);
}

TEST(config_defaults_in_range) {
    CHECK(Config{}.outOfBounds().empty());
    Config c;
    CHECK(c.apply("multiplier", "99"));  // clamped
    CHECK(c.outOfBounds().empty());
}

TEST(config_battery_saver) {
    // On battery (BDEX_POWER pretends): the automatic preset takes the
    // performance flow even on a discrete GPU; battery_saver = 2 caps the
    // multiplier; on mains nothing changes.
    const std::string path = test::tmpPath("bdex_test_battery.conf");
    {
        std::ofstream f(path);
        f << "multiplier = 4\n";
    }
    setenv("BDEX_FG_CONFIG", path.c_str(), 1);
    unsetenv("BDEX_FG_MULTIPLIER");
    VkPhysicalDeviceProperties props{};
    props.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    VkPhysicalDeviceMemoryProperties mem{};
    mem.memoryHeapCount = 1;
    mem.memoryHeaps[0].size = 8ull << 30;
    mem.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    setenv("BDEX_POWER", "mains", 1);
    Config m = Config::load();
    m.autoConfigure(props, mem);
    CHECK(!m.onBattery && m.autoTunedTo == "balanced" && m.multiplier == 4);
    setenv("BDEX_POWER", "battery", 1);
    Config b = Config::load();
    b.autoConfigure(props, mem);
    CHECK(b.onBattery && b.autoTunedTo == "performance" && b.multiplier == 4 && !b.pixelVectors);
    setenv("BDEX_FG_BATTERY_SAVER", "2", 1);
    CHECK(Config::load().multiplier == 2);
    setenv("BDEX_FG_BATTERY_SAVER", "0", 1);
    Config off = Config::load();
    off.autoConfigure(props, mem);
    CHECK(!off.onBattery && off.autoTunedTo == "balanced");
    unsetenv("BDEX_FG_BATTERY_SAVER");
    unsetenv("BDEX_POWER");
    unsetenv("BDEX_FG_CONFIG");
    remove(path.c_str());
}
