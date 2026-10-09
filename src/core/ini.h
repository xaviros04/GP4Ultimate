// Minimal INI reader shared by the mod DLL, the device tool and the native tests.
// Sections and keys are case-insensitive; ';' and '#' start comments (outside quotes);
// values are trimmed and may be wrapped in double quotes. Order is preserved.
#pragma once
#include <string>
#include <vector>
#include <utility>

namespace gp4u {

class Ini {
public:
    using Entries = std::vector<std::pair<std::string, std::string>>;

    bool load_file(const std::string& path);
    void load_string(const std::string& text);

    bool has(const std::string& sec, const std::string& key) const;
    std::string get(const std::string& sec, const std::string& key, const std::string& def = "") const;
    double get_double(const std::string& sec, const std::string& key, double def) const;
    long long get_int(const std::string& sec, const std::string& key, long long def) const;  // dec or 0x hex
    bool get_bool(const std::string& sec, const std::string& key, bool def) const;
    const Entries& section(const std::string& sec) const;  // empty if missing

    static std::string lower(std::string s);
    static std::string trim(const std::string& s);

private:
    struct Section {
        std::string name;  // lower-case
        Entries entries;   // keys lower-case, values raw (trimmed, unquoted)
    };
    std::vector<Section> sections_;
    Section* find_or_add(const std::string& name);
    const Section* find(const std::string& name) const;
};

// Parses "0x1A", "26", "-3"; returns false on garbage.
bool parse_int(const std::string& s, long long& out);
bool parse_double(const std::string& s, double& out);

}  // namespace gp4u
