#include "options_ui.h"
#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace bdex {
namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    out.push_back(trim(cur));
    return out;
}

void surfaces(const std::string& field, bool& gui, bool& launcher) {
    if (field.empty()) return;  // inherited
    gui = launcher = false;
    for (const auto& s : split(field, ',')) {
        if (s == "gui") gui = true;
        if (s == "launcher") launcher = true;
    }
}

std::string jsonString(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) {  // (a tab in a help text made the JSON invalid)
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
            o += buf;
        } else o += c;
    }
    return o + "\"";
}

std::string num(double v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

} // namespace

std::vector<UiSection> parseOptionsUi(const std::string& text, std::vector<std::string>& errors) {
    const auto infos = Config::optionInfo();
    std::vector<UiSection> sections;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const auto f = split(t, '|');
        auto err = [&](const std::string& what) {
            errors.push_back("options_ui.conf:" + std::to_string(lineNo) + ": " + what);
        };
        if (f[0] == "section") {
            if (f.size() < 6) { err("section needs 6 fields"); continue; }
            UiSection s;
            s.id = f[1];
            s.title = f[2];
            s.subtitle = f[3];
            s.expanded = f[4] == "1";
            surfaces(f[5], s.gui, s.launcher);
            sections.push_back(s);
            continue;
        }
        if (f.size() < 6) { err("option needs 6 fields"); continue; }
        if (sections.empty()) { err("option before any section"); continue; }
        UiOption o;
        o.kind = f[0];
        o.key = f[1];
        o.label = f[2];
        o.help = f[3];
        o.gui = sections.back().gui;
        o.launcher = sections.back().launcher;
        surfaces(f[5], o.gui, o.launcher);
        if (o.kind != "switch" && o.kind != "spin" && o.kind != "choice" && o.kind != "text") {
            err("unknown kind '" + o.kind + "'");
            continue;
        }
        // (An alias is kept as written: it is the key the UI reads and writes.)
        const std::string canonical = Config::canonicalKey(o.key);
        if (canonical.empty()) {
            err("'" + o.key + "' is not an option the engine knows");
            continue;
        }
        // From the registry, when the option is a plain one.
        bool ranged = false;
        for (const auto& i : infos)
            if (i.key == canonical) {
                o.def = i.def;
                o.lo = i.lo;
                o.hi = i.hi;
                o.digits = i.kind == "float" ? 2 : 0;
                o.step = i.kind == "float" ? 0.01 : 1.0;
                ranged = true;
            }
        for (const auto& kv : split(f[4], ';')) {
            if (kv.empty()) continue;
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) { err("parameter without '=': " + kv); continue; }
            const std::string k = trim(kv.substr(0, eq)), v = trim(kv.substr(eq + 1));
            if (k == "lo") { o.lo = std::strtod(v.c_str(), nullptr); ranged = true; }
            else if (k == "hi") o.hi = std::strtod(v.c_str(), nullptr);
            else if (k == "step") o.step = std::strtod(v.c_str(), nullptr);
            else if (k == "digits") o.digits = std::atoi(v.c_str());
            else if (k == "default") o.def = v;
            else if (k == "choices") {
                for (const auto& c : split(v, '/')) {
                    const size_t colon = c.find(':');
                    if (colon == std::string::npos) { err("choice without ':': " + c); continue; }
                    o.choices.emplace_back(trim(c.substr(0, colon)), trim(c.substr(colon + 1)));
                }
            } else err("unknown parameter '" + k + "'");
        }
        if (o.kind == "spin" && !ranged) { err("'" + o.key + "' has no range (give lo= and hi=)"); continue; }
        if (o.kind == "choice" && o.choices.empty()) { err("'" + o.key + "' has no choices"); continue; }
        sections.back().options.push_back(o);
    }
    return sections;
}

void applyOptionsUiEnglish(const std::string& text, std::vector<UiSection>& sections, std::vector<std::string>& errors) {
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const auto f = split(t, '|');
        auto err = [&](const std::string& what) {
            errors.push_back("options_ui.en.conf:" + std::to_string(lineNo) + ": " + what);
        };
        if (f[0] == "section") {
            if (f.size() < 4) { err("section needs 4 fields"); continue; }
            auto it = std::find_if(sections.begin(), sections.end(), [&](const UiSection& s) { return s.id == f[1]; });
            if (it == sections.end()) { err("no section '" + f[1] + "' in options_ui.conf"); continue; }
            it->titleEn = f[2];
            it->subtitleEn = f[3];
            continue;
        }
        if (f[0] != "option") { err("unknown line kind '" + f[0] + "'"); continue; }
        if (f.size() < 4) { err("option needs 4 fields"); continue; }
        UiOption* o = nullptr;
        for (auto& s : sections)
            for (auto& op : s.options)
                if (op.key == f[1]) o = &op;
        if (!o) { err("no option '" + f[1] + "' in options_ui.conf"); continue; }
        o->labelEn = f[2];
        o->helpEn = f[3];
        o->choicesEn.clear();
        const std::string choices = f.size() > 4 ? f[4] : "";
        for (const auto& c : choices.empty() ? std::vector<std::string>{} : split(choices, '/')) {
            const size_t colon = c.find(':');
            if (colon == std::string::npos) { err("choice without ':': " + c); continue; }
            o->choicesEn.push_back(trim(c.substr(colon + 1)));
            const size_t i = o->choicesEn.size() - 1;
            if (i >= o->choices.size() || o->choices[i].first != trim(c.substr(0, colon)))
                err("'" + o->key + "': choice '" + trim(c.substr(0, colon)) + "' is not options_ui.conf's (same values, same order)");
        }
        if (o->choicesEn.size() != o->choices.size())
            err("'" + o->key + "': " + std::to_string(o->choices.size()) + " choices to translate");
    }
    for (const auto& s : sections) {
        if (s.titleEn.empty()) errors.push_back("options_ui.en.conf: no English text for section '" + s.id + "'");
        for (const auto& o : s.options)
            if (o.labelEn.empty()) errors.push_back("options_ui.en.conf: no English text for '" + o.key + "'");
    }
}

void localizeOptionsUi(std::vector<UiSection>& sections, bool english) {
    if (!english) return;
    for (auto& s : sections) {
        if (!s.titleEn.empty()) { s.title = s.titleEn; s.subtitle = s.subtitleEn; }
        for (auto& o : s.options) {
            if (o.labelEn.empty()) continue;
            o.label = o.labelEn;
            o.help = o.helpEn;
            for (size_t i = 0; i < o.choices.size() && i < o.choicesEn.size(); ++i) o.choices[i].second = o.choicesEn[i];
        }
    }
}

std::string optionsUiJson(const std::vector<UiSection>& sections) {
    std::ostringstream o;
    o << "[\n";
    for (size_t s = 0; s < sections.size(); ++s) {
        const UiSection& sec = sections[s];
        o << " {\"id\": " << jsonString(sec.id) << ", \"title\": " << jsonString(sec.title)
          << ", \"subtitle\": " << jsonString(sec.subtitle) << ", \"title_en\": " << jsonString(sec.titleEn)
          << ", \"subtitle_en\": " << jsonString(sec.subtitleEn) << ", \"expanded\": " << (sec.expanded ? "true" : "false")
          << ", \"gui\": " << (sec.gui ? "true" : "false") << ", \"launcher\": " << (sec.launcher ? "true" : "false")
          << ",\n  \"options\": [\n";
        for (size_t i = 0; i < sec.options.size(); ++i) {
            const UiOption& op = sec.options[i];
            o << "   {\"kind\": " << jsonString(op.kind) << ", \"key\": " << jsonString(op.key)
              << ", \"label\": " << jsonString(op.label) << ", \"help\": " << jsonString(op.help)
              << ", \"label_en\": " << jsonString(op.labelEn) << ", \"help_en\": " << jsonString(op.helpEn)
              << ", \"lo\": " << num(op.lo) << ", \"hi\": " << num(op.hi) << ", \"step\": " << num(op.step)
              << ", \"digits\": " << op.digits << ", \"default\": " << jsonString(op.def)
              << ", \"gui\": " << (op.gui ? "true" : "false") << ", \"launcher\": " << (op.launcher ? "true" : "false")
              << ", \"choices\": [";
            for (size_t c = 0; c < op.choices.size(); ++c)
                o << (c ? ", " : "") << "[" << jsonString(op.choices[c].first) << ", " << jsonString(op.choices[c].second) << "]";
            o << "], \"choices_en\": [";
            for (size_t c = 0; c < op.choicesEn.size(); ++c) o << (c ? ", " : "") << jsonString(op.choicesEn[c]);
            o << "]}" << (i + 1 < sec.options.size() ? "," : "") << "\n";
        }
        o << "  ]}" << (s + 1 < sections.size() ? "," : "") << "\n";
    }
    o << "]\n";
    return o.str();
}

} // namespace bdex
