#include "ini_doc.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include "ini.h"

namespace gp4u {

namespace {

bool needs_quotes(const std::string& v) {
    if (v.empty()) return false;
    if (std::isspace((unsigned char)v.front()) || std::isspace((unsigned char)v.back())) return true;
    return v.find(';') != std::string::npos || v.find('#') != std::string::npos ||
           (v.size() >= 2 && v.front() == '"' && v.back() == '"');
}

std::string format_value(const std::string& v) { return needs_quotes(v) ? "\"" + v + "\"" : v; }

}  // namespace

IniDoc::Line IniDoc::parse_line(const std::string& raw, const std::string& cur_section) {
    Line l;
    l.raw = raw;
    l.section = cur_section;
    size_t a = 0;
    while (a < raw.size() && std::isspace((unsigned char)raw[a])) ++a;
    if (a == raw.size() || raw[a] == ';' || raw[a] == '#') return l;
    if (raw[a] == '[') {
        size_t e = raw.find(']', a);
        if (e == std::string::npos) return l;
        l.kind = Line::Section;
        l.section = Ini::lower(Ini::trim(raw.substr(a + 1, e - a - 1)));
        return l;
    }
    size_t eq = raw.find('=', a);
    if (eq == std::string::npos) return l;
    l.kind = Line::Key;
    l.key = Ini::lower(Ini::trim(raw.substr(a, eq - a)));
    // Comment start outside quotes (same rule as Ini::strip_comment).
    bool q = false;
    size_t c = std::string::npos;
    for (size_t i = eq + 1; i < raw.size(); ++i) {
        char ch = raw[i];
        if (ch == '"') q = !q;
        else if (!q && (ch == ';' || ch == '#')) { c = i; break; }
    }
    l.cpos = c;
    size_t vb = eq + 1, ve = c == std::string::npos ? raw.size() : c;
    while (vb < ve && std::isspace((unsigned char)raw[vb])) ++vb;
    while (ve > vb && std::isspace((unsigned char)raw[ve - 1])) --ve;
    l.vbeg = vb;
    l.vend = ve;
    std::string v = raw.substr(vb, ve - vb);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    l.value = v;
    return l;
}

void IniDoc::load_string(const std::string& text) {
    lines_.clear();
    crlf_ = text.find("\r\n") != std::string::npos;
    final_newline_ = text.empty() || text.back() == '\n';
    std::string cur;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        std::string raw = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        Line l = parse_line(raw, cur);
        if (l.kind == Line::Section) cur = l.section;
        lines_.push_back(l);
        if (e == std::string::npos) break;
        p = e + 1;
    }
}

bool IniDoc::load_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    load_string(ss.str());
    return true;
}

std::string IniDoc::text() const {
    std::string out;
    const char* nl = crlf_ ? "\r\n" : "\n";
    for (size_t i = 0; i < lines_.size(); ++i) {
        out += lines_[i].raw;
        if (i + 1 < lines_.size() || final_newline_) out += nl;
    }
    return out;
}

bool IniDoc::save_file(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    std::string t = text();
    f.write(t.data(), (std::streamsize)t.size());
    return (bool)f;
}

int IniDoc::find_key(const std::string& sec, const std::string& key) const {
    std::string s = Ini::lower(sec), k = Ini::lower(key);
    for (int i = (int)lines_.size() - 1; i >= 0; --i)
        if (lines_[i].kind == Line::Key && lines_[i].section == s && lines_[i].key == k) return i;
    return -1;
}

bool IniDoc::has(const std::string& sec, const std::string& key) const { return find_key(sec, key) >= 0; }

std::string IniDoc::get(const std::string& sec, const std::string& key, const std::string& def) const {
    int i = find_key(sec, key);
    return i < 0 ? def : lines_[i].value;
}

std::string IniDoc::comment(const std::string& sec, const std::string& key) const {
    int i = find_key(sec, key);
    if (i < 0 || lines_[i].cpos == std::string::npos) return "";
    return Ini::trim(lines_[i].raw.substr(lines_[i].cpos + 1));
}

void IniDoc::set(const std::string& sec, const std::string& key, const std::string& value) {
    std::string s = Ini::lower(sec), k = Ini::lower(key);
    std::string fv = format_value(value);
    int i = find_key(s, k);
    if (i >= 0) {
        Line& l = lines_[i];
        std::string raw = l.raw;
        size_t old_len = l.vend - l.vbeg;
        std::string tail = raw.substr(l.vend);
        // Keep an inline comment in the same column when the value shrinks or grows a bit.
        if (l.cpos != std::string::npos) {
            size_t gap = l.cpos - l.vend;   // spaces between value and ';'
            long target = (long)gap + (long)old_len - (long)fv.size();
            if (target < 1) target = 1;
            tail = std::string((size_t)target, ' ') + raw.substr(l.cpos);
        }
        l = parse_line(raw.substr(0, l.vbeg) + fv + tail, s);
        return;
    }
    // Insert after the last key of the section (before its trailing blank/comment lines).
    int last = -1, header = -1;
    for (int j = 0; j < (int)lines_.size(); ++j) {
        if (lines_[j].kind == Line::Section && lines_[j].section == s) { header = j; last = j; }
        else if (lines_[j].kind == Line::Key && lines_[j].section == s) last = j;
    }
    if (header < 0 && s.empty()) {
        // Global section: before the first section header.
        int pos = 0;
        while (pos < (int)lines_.size() && lines_[pos].kind != Line::Section) ++pos;
        lines_.insert(lines_.begin() + pos, parse_line(key + " = " + fv, ""));
        return;
    }
    if (header < 0) {
        if (!lines_.empty() && !Ini::trim(lines_.back().raw).empty()) lines_.push_back(parse_line("", lines_.back().section));
        lines_.push_back(parse_line("[" + s + "]", s));
        lines_.push_back(parse_line(key + " = " + fv, s));
        return;
    }
    lines_.insert(lines_.begin() + last + 1, parse_line(key + " = " + fv, s));
}

void IniDoc::set_comment(const std::string& sec, const std::string& key, const std::string& c) {
    int i = find_key(sec, key);
    if (i < 0) return;
    Line& l = lines_[i];
    std::string base = l.cpos == std::string::npos ? l.raw : l.raw.substr(0, l.cpos);
    while (!base.empty() && std::isspace((unsigned char)base.back())) base.pop_back();
    std::string raw = base;
    if (!c.empty()) {
        size_t col = l.cpos != std::string::npos ? l.cpos : 0;
        raw += std::string(col > raw.size() + 1 ? col - raw.size() : 1, ' ') + "; " + c;
    }
    l = parse_line(raw, l.section);
}

void IniDoc::remove(const std::string& sec, const std::string& key) {
    std::string s = Ini::lower(sec), k = Ini::lower(key);
    for (size_t i = lines_.size(); i-- > 0;)
        if (lines_[i].kind == Line::Key && lines_[i].section == s && lines_[i].key == k) lines_.erase(lines_.begin() + i);
}

std::vector<std::string> IniDoc::sections() const {
    std::vector<std::string> out;
    auto add = [&](const std::string& n) {
        for (auto& o : out)
            if (o == n) return;
        out.push_back(n);
    };
    for (auto& l : lines_) {
        if (l.kind == Line::Section) add(l.section);
        else if (l.kind == Line::Key && l.section.empty()) add("");
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> IniDoc::entries(const std::string& sec) const {
    std::string s = Ini::lower(sec);
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& l : lines_) {
        if (l.kind != Line::Key || l.section != s) continue;
        bool replaced = false;
        for (auto& kv : out)
            if (kv.first == l.key) { kv.second = l.value; replaced = true; }
        if (!replaced) out.emplace_back(l.key, l.value);
    }
    return out;
}

}  // namespace gp4u
