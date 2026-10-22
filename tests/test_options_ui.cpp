#include "options_ui.h"
#include "test.h"

#include <fstream>
#include <sstream>

using namespace bdex;

#ifndef BDEX_SOURCE_DIR  // (set by tests/CMakeLists.txt)
#define BDEX_SOURCE_DIR "."
#endif

TEST(options_ui_parses_and_fills_from_the_registry) {
    std::vector<std::string> errors;
    const auto s = parseOptionsUi(
        "# comment\n"
        "section | motion | Mouvement | Sous-titre | 1 | gui,launcher\n"
        "switch | subpixel | Sous-pixel | Aide |  | \n"
        "spin | smoothness | Lissage | Aide | lo=0;hi=0.03;step=0.001;digits=3 | gui\n"
        "choice | debug | Débogage | | choices=none:Aucun/flow:Flux;default=none | \n"
        "switch | no_such_key | X | Y |  | \n",
        errors);
    CHECK(errors.size() == 1 && errors[0].find("no_such_key") != std::string::npos);
    CHECK(s.size() == 1 && s[0].title == "Mouvement" && s[0].expanded && s[0].launcher);
    CHECK(s[0].options.size() == 3);
    const UiOption& sub = s[0].options[0];
    CHECK(sub.kind == "switch" && sub.def == "0" && sub.gui && sub.launcher);   // default from the registry
    const UiOption& sm = s[0].options[1];
    CHECK(sm.def == "0.003" && sm.hi == 0.03 && sm.digits == 3 && sm.gui && !sm.launcher);
    CHECK(s[0].options[2].choices.size() == 2 && s[0].options[2].def == "none");
    const std::string json = optionsUiJson(s);
    CHECK(json.find("\"key\": \"smoothness\"") != std::string::npos && json.find("\"Débogage\"") != std::string::npos);
}

TEST(options_ui_the_shipped_file_is_valid) {
    std::ifstream in(std::string(BDEX_SOURCE_DIR) + "/layer/options_ui.conf");
    CHECK(bool(in));
    std::stringstream text;
    text << in.rdbuf();
    std::vector<std::string> errors;
    const auto s = parseOptionsUi(text.str(), errors);
    for (const auto& e : errors) fprintf(stderr, "  %s\n", e.c_str());
    CHECK(errors.empty() && s.size() >= 6);
}

TEST(options_ui_every_option_has_english_texts) {
    auto read = [](const char* name) {
        std::ifstream in(std::string(BDEX_SOURCE_DIR) + "/layer/" + name);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    };
    std::vector<std::string> errors;
    auto s = parseOptionsUi(read("options_ui.conf"), errors);
    applyOptionsUiEnglish(read("options_ui.en.conf"), s, errors);
    for (const auto& e : errors) fprintf(stderr, "  %s\n", e.c_str());
    CHECK(errors.empty());
    localizeOptionsUi(s, true);
    CHECK(s[0].title == "Pacing");
    bool choices = false;
    for (const auto& sec : s)
        for (const auto& o : sec.options)
            if (o.key == "debug") choices = o.choices.size() == 4 && o.choices[2].second == "Split screen";
    CHECK(choices);
    // A missing translation is an error.
    auto t = parseOptionsUi(read("options_ui.conf"), errors);
    errors.clear();
    applyOptionsUiEnglish("section | cadence | Pacing | x\n", t, errors);
    CHECK(!errors.empty());
}
