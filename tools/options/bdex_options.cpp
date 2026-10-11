// bdex_options <options_ui.conf> <out.json>: the option controls of the user
// interfaces (layer/options_ui.conf) completed from the option registry
// (kind, range, default), as JSON for the GTK panel. Fails on any option the
// engine does not know, so the build catches a stale UI description.
#include "options_ui.h"

#include <cstdio>
#include <fstream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <options_ui.conf> <options_ui.en.conf> <out.json>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        fprintf(stderr, "bdex_options: cannot read %s\n", argv[1]);
        return 1;
    }
    std::stringstream text;
    text << in.rdbuf();
    std::vector<std::string> errors;
    auto sections = bdex::parseOptionsUi(text.str(), errors);
    std::ifstream en(argv[2]);
    if (!en) {
        fprintf(stderr, "bdex_options: cannot read %s\n", argv[2]);
        return 1;
    }
    std::stringstream enText;
    enText << en.rdbuf();
    bdex::applyOptionsUiEnglish(enText.str(), sections, errors);
    for (const auto& e : errors) fprintf(stderr, "%s\n", e.c_str());
    if (!errors.empty()) return 1;
    std::ofstream out(argv[3], std::ios::trunc);
    out << bdex::optionsUiJson(sections);
    return out ? 0 : 1;
}
