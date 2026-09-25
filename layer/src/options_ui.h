#pragma once
// The option controls of the user interfaces (the GTK panel, the Windows
// launcher), described once in layer/options_ui.conf: sections, labels,
// help texts, UI ranges. What the registry knows (kind, range, default) is
// filled in from Config::optionInfo, so the UIs never restate it.
#include <string>
#include <utility>
#include <vector>

namespace bdex {

struct UiOption {
    std::string kind;   // "switch", "spin", "choice", "text"
    std::string key, label, help;
    double lo = 0.0, hi = 0.0, step = 1.0;
    int digits = 0;
    std::string def;                                          // default config value
    std::vector<std::pair<std::string, std::string>> choices; // (value, label)
    bool gui = true, launcher = false;
};

struct UiSection {
    std::string id, title, subtitle;
    bool expanded = false;
    bool gui = true, launcher = false;
    std::vector<UiOption> options;
};

// Parses the file's text and fills what it leaves out from the registry.
// Problems (a key the engine does not know, a malformed line, a spin without
// a range) go to `errors`; the options concerned are left out.
std::vector<UiSection> parseOptionsUi(const std::string& text, std::vector<std::string>& errors);

// The sections as JSON (tools/options/bdex_options.cpp, for the GTK panel).
std::string optionsUiJson(const std::vector<UiSection>& sections);

} // namespace bdex
