// Keeps the configurator honest across upgrades:
//  - config/gp4ultimate_schema.ini loads cleanly and is self-consistent;
//  - every option the DLLs read (ini.get*("section", "key", default) in src/, plus every
//    FfbParams field) is described, with the same default as the code;
//  - the template gp4ultimate_reference.ini round-trips byte for byte through IniDoc, only documents
//    known options, and its mapping parses.
//   test_schema <repository root>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "core/ffb_model.h"
#include "core/ini.h"
#include "core/ini_doc.h"
#include "core/mapping.h"
#include "core/schema.h"

using namespace gp4u;
namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, what) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL: %s\n", std::string(what).c_str()); } } while (0)

static std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool same_number(double a, double b) { return a == b || std::fabs(a - b) <= 1e-12 * std::max(std::fabs(a), std::fabs(b)); }

int main(int argc, char** argv) {
    fs::path mod = argc > 1 ? argv[1] : ".";
    std::string schema_text = read(mod / "config" / "gp4ultimate_schema.ini");
    CHECK(!schema_text.empty(), "schema file readable");
    Schema sc;
    std::vector<std::string> warn;
    CHECK(sc.load(schema_text, &warn), "schema loads");
    for (auto& w : warn) CHECK(false, "schema warning: " + w);
    CHECK(sc.version >= 1, "schema version");
    {
        // The Spanish view: every page and group has a Spanish name, labels are Spanish.
        Schema es;
        CHECK(es.load(schema_text, nullptr, "es"), "schema loads in Spanish");
        CHECK(es.items.size() == sc.items.size(), "same options in Spanish");
        for (size_t i = 0; i < es.items.size() && i < sc.items.size(); ++i) {
            const auto& e = es.items[i];
            CHECK(e.page == sc.items[i].page, e.key + ": page id independent of the language");
            if (e.page != "Force feedback" && e.page != "Online")
                CHECK(e.page_label != e.page, e.section + "." + e.key + ": page '" + e.page + "' has no Spanish name");
            if (!e.group.empty() && e.group != "General" && e.group != "Force feedback")
                CHECK(e.group_label != e.group, e.section + "." + e.key + ": group '" + e.group + "' has no Spanish name");
        }
        const SchemaItem* m = es.find("ffb", "mode");
        CHECK(m && m->label == "Origen de la fuerza" && m->choices.size() == 4 && m->choices[0].first == "telemetry" &&
                  m->choices[0].second == "Telemetría (recomendado)",
              "Spanish label and choices");
        const SchemaItem* me = sc.find("ffb", "mode");
        CHECK(me && me->label == "Force source" && me->choices[0].second == "Telemetry (recommended)", "English label and choices");
    }

    // Self-consistency.
    IniDoc raw;
    raw.load_string(schema_text);
    for (auto& it : sc.items) {
        std::string id = it.section + "." + it.key;
        for (const char* f : {"label", "help", "choices", "default", "label_es", "help_es", "choices_es"})
            CHECK(raw.comment(id, f).empty(), id + ": '" + f + "' contains ';' or '#' (would be cut)");
        // Spanish translation complete (English is the base language).
        CHECK(raw.has(id, "label_es"), id + ": no label_es");
        if (raw.has(id, "help")) CHECK(raw.has(id, "help_es"), id + ": no help_es");
        if (raw.has(id, "choices")) CHECK(raw.has(id, "choices_es"), id + ": no choices_es");
        CHECK(!it.page.empty() && !it.label.empty(), id + ": page and label");
        double d;
        if (it.type == "float" || it.type == "int") {
            CHECK(parse_double(it.def, d), id + ": numeric default");
            if (it.has_min) CHECK(d >= it.min, id + ": default below min");
            if (it.has_max) CHECK(d <= it.max, id + ": default above max");
        } else if (it.type == "hex") {
            long long v;
            CHECK(parse_int(it.def, v), id + ": hex default");
        } else if (it.type == "bool") {
            CHECK(it.def == "0" || it.def == "1", id + ": bool default 0/1");
        } else if (it.type == "choice") {
            bool ok = false;
            for (auto& c : it.choices) ok |= c.first == Ini::lower(it.def);
            CHECK(ok, id + ": default among choices");
        }
        int dup = 0;
        for (auto& o : sc.items) dup += o.section == it.section && o.key == it.key;
        CHECK(dup == 1, id + ": described once");
    }

    // Every FfbParams field, with the code's default.
    FfbParams p;
    p.for_each([&](const char* k, auto& v) {
        const SchemaItem* it = sc.find("ffb", k);
        CHECK(it != nullptr, std::string("ffb.") + k + " missing from the schema");
        if (!it) return;
        double d = 0;
        if (it->type == "bool") d = it->def == "1";
        else parse_double(it->def, d);
        CHECK(same_number(d, (double)v), std::string("ffb.") + k + ": schema default " + it->def + " != code " + format_number((double)v));
    });

    // Every ini.get*("section", "key", default) in the sources.
    std::regex call(R"re(get(_bool|_int|_double)?\(\s*"([a-z_]+)"\s*,\s*"([a-z_0-9]+)"\s*(?:,\s*([^)]*))?\))re");
    std::set<std::string> seen;
    for (auto& e : fs::recursive_directory_iterator(mod / "src")) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        std::string src = read(e.path());
        for (std::sregex_iterator m(src.begin(), src.end(), call), end; m != end; ++m) {
            std::string kind = (*m)[1], sec = (*m)[2], key = (*m)[3], def = Ini::trim((*m)[4]);
            std::string id = sec + "." + key;
            if (!seen.insert(id).second) continue;
            const SchemaItem* it = sc.find(sec, key);
            CHECK(it || sc.managed(sec), id + " (read in " + e.path().filename().string() + ") missing from the schema");
            if (!it || def.empty()) continue;
            double a, b;
            long long ia;
            if (def == "true" || def == "false") {
                CHECK(it->def == (def == "true" ? "1" : "0"), id + ": default " + it->def + " != code " + def);
            } else if (def.front() == '"' && def.back() == '"') {
                CHECK(Ini::lower(it->def) == Ini::lower(def.substr(1, def.size() - 2)), id + ": default '" + it->def + "' != code " + def);
            } else if (parse_double(def, a) && (parse_double(it->def, b) || (parse_int(it->def, ia) && ((b = (double)ia), true)))) {
                CHECK(same_number(a, b), id + ": default " + it->def + " != code " + def);
            }   // anything else is an expression (VK_F11, known_ ? ...): not compared
        }
    }
    CHECK(seen.size() > 30, "found the DLLs' ini reads (" + std::to_string(seen.size()) + ")");

    // The template.
    std::string tpl = read(mod / "config" / "gp4ultimate_reference.ini");
    IniDoc doc;
    doc.load_string(tpl);
    CHECK(doc.text() == tpl, "template round-trips byte for byte");
    for (auto& s : doc.sections())
        if (!sc.managed(s))
            for (auto& kv : doc.entries(s)) CHECK(sc.find(s, kv.first) != nullptr, "template option " + s + "." + kv.first + " not in the schema");
    // The template documents the defaults: an explicit different value there would pin
    // every new user to it and hide later improvements of the default.
    for (auto& s : doc.sections()) {
        if (sc.managed(s)) continue;
        for (auto& kv : doc.entries(s)) {
            const SchemaItem* it = sc.find(s, kv.first);
            if (!it || it->def.empty()) continue;
            double a, b;
            long long ia, ib;
            bool same = kv.second == it->def || (parse_double(kv.second, a) && parse_double(it->def, b) && same_number(a, b)) ||
                        (parse_int(kv.second, ia) && parse_int(it->def, ib) && ia == ib) ||
                        (it->type == "bool" && ((kv.second == "1") == (it->def == "1")));
            CHECK(same, "template " + s + "." + kv.first + " = " + kv.second + " but the default is " + it->def);
        }
    }
    Ini ini;
    ini.load_string(tpl);
    MappingConfig m;
    std::string err;
    CHECK(parse_mapping(ini, m, err), "template mapping parses: " + err);
    // IniDoc reads what Ini reads.
    for (auto& s : doc.sections())
        for (auto& kv : doc.entries(s)) CHECK(ini.get(s, kv.first) == kv.second, "IniDoc/Ini agree on " + s + "." + kv.first);

    std::printf("[schema] %d passed, %d failed (%zu options described, %zu read by the DLLs)\n", g_pass, g_fail,
                sc.items.size(), seen.size());
    return g_fail ? 1 : 0;
}
