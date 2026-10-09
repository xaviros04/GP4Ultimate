#include "ini.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gp4u {

std::string Ini::lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Ini::trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string strip_comment(const std::string& line) {
    bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') q = !q;
        else if (!q && (c == ';' || c == '#')) return line.substr(0, i);
    }
    return line;
}

static std::string unquote(const std::string& v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
    return v;
}

Ini::Section* Ini::find_or_add(const std::string& name) {
    for (auto& s : sections_)
        if (s.name == name) return &s;
    sections_.push_back({name, {}});
    return &sections_.back();
}

const Ini::Section* Ini::find(const std::string& name) const {
    std::string n = lower(name);
    for (auto& s : sections_)
        if (s.name == n) return &s;
    return nullptr;
}

void Ini::load_string(const std::string& text) {
    sections_.clear();
    std::istringstream in(text);
    std::string line;
    Section* cur = find_or_add("");
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[') {
            size_t e = t.find(']');
            if (e != std::string::npos) cur = find_or_add(lower(trim(t.substr(1, e - 1))));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(t.substr(0, eq)));
        std::string val = unquote(trim(strip_comment(t.substr(eq + 1))));
        bool replaced = false;
        for (auto& kv : cur->entries)
            if (kv.first == key) { kv.second = val; replaced = true; break; }
        if (!replaced) cur->entries.emplace_back(key, val);
    }
}

bool Ini::load_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    load_string(ss.str());
    return true;
}

bool Ini::has(const std::string& sec, const std::string& key) const {
    const Section* s = find(sec);
    if (!s) return false;
    std::string k = lower(key);
    for (auto& kv : s->entries)
        if (kv.first == k) return true;
    return false;
}

std::string Ini::get(const std::string& sec, const std::string& key, const std::string& def) const {
    const Section* s = find(sec);
    if (!s) return def;
    std::string k = lower(key);
    for (auto& kv : s->entries)
        if (kv.first == k) return kv.second;
    return def;
}

bool parse_int(const std::string& s0, long long& out) {
    std::string s = Ini::trim(s0);
    if (s.empty()) return false;
    char* end = nullptr;
    int base = 10;
    const char* p = s.c_str();
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    if (!*p) return false;
    unsigned long long v = std::strtoull(p, &end, base);
    if (end == p || *end) return false;
    out = neg ? -(long long)v : (long long)v;
    return true;
}

bool parse_double(const std::string& s0, double& out) {
    std::string s = Ini::trim(s0);
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && !*end;
}

double Ini::get_double(const std::string& sec, const std::string& key, double def) const {
    double v;
    return parse_double(get(sec, key), v) ? v : def;
}

long long Ini::get_int(const std::string& sec, const std::string& key, long long def) const {
    long long v;
    return parse_int(get(sec, key), v) ? v : def;
}

bool Ini::get_bool(const std::string& sec, const std::string& key, bool def) const {
    std::string v = lower(get(sec, key));
    if (v == "1" || v == "true" || v == "yes" || v == "on" || v == "si" || v == "sí") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

const Ini::Entries& Ini::section(const std::string& sec) const {
    static const Entries empty;
    const Section* s = find(sec);
    return s ? s->entries : empty;
}

}  // namespace gp4u
