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
        else o += c;
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

std::string optionsUiJson(const std::vector<UiSection>& sections) {
    std::ostringstream o;
    o << "[\n";
    for (size_t s = 0; s < sections.size(); ++s) {
        const UiSection& sec = sections[s];
        o << " {\"id\": " << jsonString(sec.id) << ", \"title\": " << jsonString(sec.title)
          << ", \"subtitle\": " << jsonString(sec.subtitle) << ", \"expanded\": " << (sec.expanded ? "true" : "false")
          << ", \"gui\": " << (sec.gui ? "true" : "false") << ", \"launcher\": " << (sec.launcher ? "true" : "false")
          << ",\n  \"options\": [\n";
        for (size_t i = 0; i < sec.options.size(); ++i) {
            const UiOption& op = sec.options[i];
            o << "   {\"kind\": " << jsonString(op.kind) << ", \"key\": " << jsonString(op.key)
              << ", \"label\": " << jsonString(op.label) << ", \"help\": " << jsonString(op.help)
              << ", \"lo\": " << num(op.lo) << ", \"hi\": " << num(op.hi) << ", \"step\": " << num(op.step)
              << ", \"digits\": " << op.digits << ", \"default\": " << jsonString(op.def)
              << ", \"gui\": " << (op.gui ? "true" : "false") << ", \"launcher\": " << (op.launcher ? "true" : "false")
              << ", \"choices\": [";
            for (size_t c = 0; c < op.choices.size(); ++c)
                o << (c ? ", " : "") << "[" << jsonString(op.choices[c].first) << ", " << jsonString(op.choices[c].second) << "]";
            o << "]}" << (i + 1 < sec.options.size() ? "," : "") << "\n";
        }
        o << "  ]}" << (s + 1 < sections.size() ? "," : "") << "\n";
    }
    o << "]\n";
    return o.str();
}

} // namespace bdex
