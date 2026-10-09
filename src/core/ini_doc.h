// Round-trip INI document for the configurator: edits values in place and keeps everything
// else byte for byte (comments, blank lines, order, alignment of inline comments, line
// endings, keys it does not know). Reading rules match core/ini.h exactly (case-insensitive
// names, ';'/'#' comments outside quotes, quoted values, last duplicate wins), so what the
// configurator shows is what the DLLs will read.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace gp4u {

class IniDoc {
public:
    void load_string(const std::string& text);
    bool load_file(const std::string& path);
    std::string text() const;
    bool save_file(const std::string& path) const;

    bool has(const std::string& sec, const std::string& key) const;
    std::string get(const std::string& sec, const std::string& key, const std::string& def = "") const;
    // Inline comment of a key line, without the ';' and surrounding spaces ("" if none).
    std::string comment(const std::string& sec, const std::string& key) const;

    // Sets a value (and optionally the inline comment); adds the key at the end of its
    // section, or the section at the end of the file, if needed.
    void set(const std::string& sec, const std::string& key, const std::string& value);
    void set_comment(const std::string& sec, const std::string& key, const std::string& comment);
    void remove(const std::string& sec, const std::string& key);   // every occurrence

    std::vector<std::string> sections() const;   // lower-case, in file order, unique
    // Keys of a section in file order (last value of duplicates), lower-case.
    std::vector<std::pair<std::string, std::string>> entries(const std::string& sec) const;

private:
    struct Line {
        std::string raw;
        enum Kind { Other, Section, Key } kind = Other;
        std::string section;   // lower-case section this line belongs to (Key) or opens (Section)
        std::string key;       // lower-case
        std::string value;     // trimmed, unquoted
        size_t vbeg = 0, vend = 0;   // raw value span (trimmed, quotes included)
        size_t cpos = std::string::npos;   // start of the inline comment
    };
    std::vector<Line> lines_;
    bool crlf_ = false;
    bool final_newline_ = true;

    static Line parse_line(const std::string& raw, const std::string& cur_section);
    int find_key(const std::string& sec, const std::string& key) const;   // last occurrence
};

}  // namespace gp4u
