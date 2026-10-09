// Description of gp4ultimate.ini's options for the configurator (config/gp4ultimate_schema.ini).
// Pure C++: loaded by gp4ultimate-config.exe and checked by test_schema against what the DLLs read.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace gp4u {

struct SchemaItem {
    std::string section, key;      // lower-case
    std::string type = "text";     // bool | int | float | text | choice | hex (unknown -> text)
    std::string label, help, unit, def;          // in the loaded language
    std::string page, group;                     // ids (English, as written in the schema)
    std::string page_label, group_label;         // in the loaded language
    double min = 0, max = 0, step = 0;
    bool has_min = false, has_max = false, advanced = false;
    std::vector<std::pair<std::string, std::string>> choices;   // value, label
};

struct SchemaSection {   // [@name]
    std::string name, managed, help;   // help in the loaded language
};

struct Schema {
    int version = 0;
    std::vector<SchemaItem> items;          // file order
    std::vector<SchemaSection> sections;
    std::vector<std::string> pages;         // ids, order of first appearance
    std::string lang = "en";

    // Returns false only for text that is not a schema at all; per-item problems are
    // reported in `warnings` and the item is kept with safe fallbacks.
    //
    // Languages: label/help/choices/unit are English; `label_<lang>`, `help_<lang>`,
    // `choices_<lang>`, `unit_<lang>` override them for another language, and the section
    // [@lang.<lang>] translates page and group names (English name = translation).
    // Anything without a translation stays in English.
    bool load(const std::string& text, std::vector<std::string>* warnings = nullptr, const std::string& lang = "en");
    // Page/group name in the loaded language.
    std::string tr(const std::string& english) const;
    const SchemaItem* find(const std::string& sec, const std::string& key) const;
    const SchemaSection* section(const std::string& name) const;
    bool managed(const std::string& sec) const;   // edited by the Controls page

private:
    std::vector<std::pair<std::string, std::string>> names_;   // lower-case English -> translation
};

// Canonical text for a number in the ini (no trailing zeros, "1" not "1.000000").
std::string format_number(double v);

}  // namespace gp4u
