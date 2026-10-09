#include "schema.h"

#include <cmath>
#include <cstdio>

#include "ini.h"
#include "ini_doc.h"

namespace gp4u {

std::string format_number(double v) {
    if (std::isfinite(v) && std::fabs(v) < 1e15 && v == std::floor(v)) {
        char b[32];
        std::snprintf(b, sizeof b, "%.0f", v);
        return b;
    }
    char b[40];
    for (int prec = 6; prec <= 17; ++prec) {   // shortest text that reads back exactly
        std::snprintf(b, sizeof b, "%.*g", prec, v);
        double back;
        if (parse_double(b, back) && back == v) break;
    }
    return b;
}

namespace {
std::vector<std::pair<std::string, std::string>> parse_choices(const std::string& c) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t p = 0;
    while (p <= c.size()) {
        size_t bar = c.find('|', p);
        std::string part = Ini::trim(c.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
        if (!part.empty()) {
            size_t colon = part.find(':');
            std::string v = Ini::trim(colon == std::string::npos ? part : part.substr(0, colon));
            std::string l = colon == std::string::npos ? v : Ini::trim(part.substr(colon + 1));
            out.emplace_back(Ini::lower(v), l);
        }
        if (bar == std::string::npos) break;
        p = bar + 1;
    }
    return out;
}
}  // namespace

std::string Schema::tr(const std::string& english) const {
    std::string k = Ini::lower(english);
    for (auto& n : names_)
        if (n.first == k) return n.second;
    return english;
}

bool Schema::load(const std::string& text, std::vector<std::string>* warnings, const std::string& language) {
    *this = Schema();
    lang = Ini::lower(language.empty() ? std::string("en") : language);
    IniDoc doc;
    doc.load_string(text);
    // Translated text for `field` of section `name`, else the English one.
    auto loc = [&](const std::string& name, const std::string& field, const std::string& def = "") {
        if (lang != "en" && doc.has(name, field + "_" + lang)) return doc.get(name, field + "_" + lang);
        return doc.get(name, field, def);
    };
    if (lang != "en") {
        std::string sec = "@lang." + lang;
        for (auto& kv : doc.entries(sec)) names_.emplace_back(kv.first, kv.second);
    }
    auto warn = [&](const std::string& w) { if (warnings) warnings->push_back(w); };
    bool any = false;
    for (const std::string& name : doc.sections()) {
        if (name.empty()) continue;
        if (name == "schema") {
            long long v;
            if (parse_int(doc.get(name, "version", "0"), v)) version = (int)v;
            any = true;
            continue;
        }
        if (name.rfind("@lang.", 0) == 0) continue;
        if (name[0] == '@') {
            sections.push_back({name.substr(1), Ini::lower(doc.get(name, "managed")), loc(name, "help")});
            any = true;
            continue;
        }
        size_t dot = name.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) {
            warn("[" + name + "]: expected [section.key]");
            continue;
        }
        any = true;
        SchemaItem it;
        it.section = name.substr(0, dot);
        it.key = name.substr(dot + 1);
        it.type = Ini::lower(doc.get(name, "type", "text"));
        if (it.type != "bool" && it.type != "int" && it.type != "float" && it.type != "text" && it.type != "choice" &&
            it.type != "hex") {
            warn("[" + name + "]: unknown type '" + it.type + "', shown as text");
            it.type = "text";
        }
        it.label = loc(name, "label", it.key);
        it.help = loc(name, "help");
        it.page = doc.get(name, "page", "Advanced");
        it.group = doc.get(name, "group", "");
        it.page_label = tr(it.page);
        it.group_label = tr(it.group);
        it.unit = loc(name, "unit");
        it.def = doc.get(name, "default");
        it.advanced = doc.get(name, "advanced", "0") == "1";
        double d;
        if (doc.has(name, "min") && parse_double(doc.get(name, "min"), d)) { it.min = d; it.has_min = true; }
        if (doc.has(name, "max") && parse_double(doc.get(name, "max"), d)) { it.max = d; it.has_max = true; }
        if (doc.has(name, "step") && parse_double(doc.get(name, "step"), d)) it.step = d;
        if (it.type == "choice") {
            it.choices = parse_choices(doc.get(name, "choices"));
            if (lang != "en" && doc.has(name, "choices_" + lang)) {   // same values, translated labels
                auto t = parse_choices(doc.get(name, "choices_" + lang));
                for (auto& c : it.choices)
                    for (auto& x : t)
                        if (x.first == c.first) c.second = x.second;
            }
            if (it.choices.empty()) {
                warn("[" + name + "]: choice without choices, shown as text");
                it.type = "text";
            }
        }
        bool seen = false;
        for (auto& pg : pages) seen |= pg == it.page;
        if (!seen) pages.push_back(it.page);
        items.push_back(it);
    }
    return any;
}

const SchemaItem* Schema::find(const std::string& sec, const std::string& key) const {
    std::string s = Ini::lower(sec), k = Ini::lower(key);
    for (auto& i : items)
        if (i.section == s && i.key == k) return &i;
    return nullptr;
}

const SchemaSection* Schema::section(const std::string& name) const {
    std::string n = Ini::lower(name);
    for (auto& s : sections)
        if (s.name == n) return &s;
    return nullptr;
}

bool Schema::managed(const std::string& sec) const {
    const SchemaSection* s = section(sec);
    return s && !s->managed.empty();
}

}  // namespace gp4u
