#include "mapping.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace gp4u {

static const char* kAxisNames[AX_COUNT] = {"X", "Y", "Z", "RX", "RY", "RZ", "S0", "S1"};

const char* axis_name(int a) { return (a >= 0 && a < AX_COUNT) ? kAxisNames[a] : "?"; }

bool parse_axis_name(const std::string& s0, int& out) {
    std::string s = Ini::lower(Ini::trim(s0));
    static const struct { const char* n; int a; } names[] = {
        {"x", AX_X},   {"y", AX_Y},   {"z", AX_Z},   {"rx", AX_RX},      {"ry", AX_RY},
        {"rz", AX_RZ}, {"s0", AX_S0}, {"s1", AX_S1}, {"slider0", AX_S0}, {"slider1", AX_S1},
    };
    for (auto& n : names)
        if (s == n.n) { out = n.a; return true; }
    return false;
}

const NamedDevice* MappingConfig::device(const std::string& alias) const {
    std::string a = Ini::lower(alias);
    for (auto& d : devices)
        if (d.alias == a) return &d;
    return nullptr;
}

static std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string tok;
    while (in >> tok) out.push_back(tok);
    return out;
}

static std::vector<std::string> split_list(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(Ini::trim(cur)); cur.clear(); }
        else cur += c;
    }
    if (!Ini::trim(cur).empty()) out.push_back(Ini::trim(cur));
    return out;
}

bool parse_selector(const std::string& text0, DeviceSelector& out, std::string& err) {
    std::string text = Ini::trim(text0);
    size_t colon = text.find(':');
    if (colon == std::string::npos) { err = "device selector needs a kind (name:, vidpid:, guid:, index:): " + text; return false; }
    std::string kind = Ini::lower(Ini::trim(text.substr(0, colon)));
    std::string rest = Ini::trim(text.substr(colon + 1));
    if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') rest = rest.substr(1, rest.size() - 2);
    out = DeviceSelector{};
    if (kind == "name") {
        if (rest.empty()) { err = "empty device name"; return false; }
        out.kind = DeviceSelector::Name;
        out.text = Ini::lower(rest);
        return true;
    }
    if (kind == "vidpid") {
        size_t c2 = rest.find(':');
        if (c2 == std::string::npos) { err = "vidpid must be VVVV:PPPP (hex): " + rest; return false; }
        char* e1 = nullptr; char* e2 = nullptr;
        std::string v = rest.substr(0, c2), p = rest.substr(c2 + 1);
        unsigned long vv = std::strtoul(v.c_str(), &e1, 16), pp = std::strtoul(p.c_str(), &e2, 16);
        if (v.empty() || p.empty() || *e1 || *e2 || vv > 0xFFFF || pp > 0xFFFF) { err = "bad vidpid: " + rest; return false; }
        out.kind = DeviceSelector::VidPid;
        out.vid = (uint16_t)vv; out.pid = (uint16_t)pp;
        return true;
    }
    if (kind == "guid") {
        std::string g = rest;
        for (auto& c : g) c = (char)std::toupper((unsigned char)c);
        if (g.size() != 38 || g.front() != '{' || g.back() != '}') { err = "guid must look like {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"; return false; }
        out.kind = DeviceSelector::Guid;
        out.text = g;
        return true;
    }
    if (kind == "index") {
        long long n;
        if (!parse_int(rest, n) || n < 0 || n > 63) { err = "bad index: " + rest; return false; }
        out.kind = DeviceSelector::Index;
        out.index = (int)n;
        return true;
    }
    err = "unknown device selector kind '" + kind + "'";
    return false;
}

bool selector_matches(const DeviceSelector& sel, const std::string& product_name, const std::string& instance_name,
                      uint16_t vid, uint16_t pid, const std::string& guid, int index) {
    switch (sel.kind) {
        case DeviceSelector::VidPid: return vid == sel.vid && pid == sel.pid;
        case DeviceSelector::Name:
            return Ini::lower(product_name).find(sel.text) != std::string::npos ||
                   Ini::lower(instance_name).find(sel.text) != std::string::npos;
        case DeviceSelector::Guid: return guid == sel.text;
        case DeviceSelector::Index: return index == sel.index;
    }
    return false;
}

static bool parse_option(const std::string& tok, AxisSource& s, std::string& err) {
    std::string t = Ini::lower(tok);
    if (t == "invert" || t == "inv") { s.invert = true; return true; }
    if (t == "unipolar") return true;   // handled by the binding (steering without centre)
    if (t == "center" || t == "centre" || t == "bipolar") { s.bipolar = true; return true; }
    size_t eq = t.find('=');
    if (eq == std::string::npos) { err = "unknown axis option '" + tok + "'"; return false; }
    std::string k = t.substr(0, eq);
    double v;
    if (!parse_double(t.substr(eq + 1), v)) { err = "bad number in '" + tok + "'"; return false; }
    if (k == "deadzone" || k == "dz") { if (v < 0 || v >= 1) { err = "deadzone must be in [0,1)"; return false; } s.deadzone = v; }
    else if (k == "gamma") { if (v <= 0 || v > 10) { err = "gamma must be in (0,10]"; return false; } s.gamma = v; }
    else if (k == "min") { if (v < 0 || v > 1) { err = "min must be in [0,1]"; return false; } s.cal_min = v; }
    else if (k == "max") { if (v < 0 || v > 1) { err = "max must be in [0,1]"; return false; } s.cal_max = v; }
    else { err = "unknown axis option '" + k + "'"; return false; }
    return true;
}

bool parse_axis_source(const std::string& text, AxisSource& out, std::string& err) {
    std::vector<std::string> toks = split_ws(text);
    if (toks.empty()) { err = "empty axis source"; return false; }
    size_t dot = toks[0].find('.');
    if (dot == std::string::npos) { err = "axis source must be alias.AXIS: " + toks[0]; return false; }
    out = AxisSource{};
    out.device = Ini::lower(toks[0].substr(0, dot));
    if (!parse_axis_name(toks[0].substr(dot + 1), out.axis)) { err = "unknown axis '" + toks[0].substr(dot + 1) + "'"; return false; }
    for (size_t i = 1; i < toks.size(); ++i)
        if (!parse_option(toks[i], out, err)) return false;
    if (out.cal_max <= out.cal_min) { err = "max must be greater than min"; return false; }
    return true;
}

static bool parse_axis_binding(int target, const std::string& value, AxisBinding& b, std::string& err) {
    b = AxisBinding{};
    b.target = target;
    std::string v = Ini::trim(value);
    std::string lv = Ini::lower(v);
    if (lv.rfind("combine(", 0) == 0) {
        if (v.back() != ')') { err = "combine(...) not closed"; return false; }
        std::vector<std::string> parts = split_list(v.substr(8, v.size() - 9), ',');
        if (parts.size() != 2) { err = "combine needs two sources: combine(accel, brake)"; return false; }
        b.kind = AxisBinding::Combine;
        return parse_axis_source(parts[0], b.a, err) && parse_axis_source(parts[1], b.b, err);
    }
    b.kind = AxisBinding::Single;
    if (!parse_axis_source(v, b.a, err)) return false;
    // Steering is centre-referenced unless told otherwise.
    if (target == AX_X && lv.find("unipolar") == std::string::npos) b.a.bipolar = true;
    return true;
}

// "wheel.b4" / "wheel.button4" / "wheel.pov0"
static bool parse_obj_ref(const std::string& text, const char* prefix_short, const char* prefix_long,
                          std::string& dev, int& idx, std::string& err) {
    std::string t = Ini::lower(Ini::trim(text));
    size_t dot = t.find('.');
    if (dot == std::string::npos) { err = "expected alias." + std::string(prefix_short) + "N: " + text; return false; }
    dev = t.substr(0, dot);
    std::string o = t.substr(dot + 1);
    std::string num;
    if (o.rfind(prefix_long, 0) == 0) num = o.substr(strlen(prefix_long));
    else if (o.rfind(prefix_short, 0) == 0) num = o.substr(strlen(prefix_short));
    else { err = "expected " + std::string(prefix_short) + "N after the alias: " + text; return false; }
    long long n;
    if (!parse_int(num, n) || n < 0 || n > 127) { err = "bad index in " + text; return false; }
    idx = (int)n;
    return true;
}

bool parse_mapping(const Ini& ini, MappingConfig& out, std::string& err, std::vector<std::string>* warnings) {
    out = MappingConfig{};
    for (auto& kv : ini.section("devices")) {
        NamedDevice d;
        d.alias = kv.first;
        if (!parse_selector(kv.second, d.sel, err)) { err = "[devices] " + kv.first + ": " + err; return false; }
        out.devices.push_back(d);
    }
    out.virtual_name = ini.get("virtual", "name", out.virtual_name);
    out.ffb_device = Ini::lower(ini.get("virtual", "ffb", ""));
    out.hide_physical = ini.get_bool("virtual", "hide_physical", true);
    out.hide_others = ini.get_bool("virtual", "hide_others", true);
    out.virtual_first = ini.get_bool("virtual", "virtual_first", true);
    if (!out.ffb_device.empty() && !out.device(out.ffb_device)) {
        err = "[virtual] ffb = " + out.ffb_device + " is not a [devices] alias";
        return false;
    }

    bool used[AX_COUNT] = {};
    for (auto& kv : ini.section("axes")) {
        int target;
        if (!parse_axis_name(kv.first, target)) { err = "[axes] unknown virtual axis '" + kv.first + "'"; return false; }
        if (used[target]) { err = std::string("[axes] ") + axis_name(target) + " bound twice"; return false; }
        AxisBinding b;
        if (!parse_axis_binding(target, kv.second, b, err)) { err = "[axes] " + kv.first + ": " + err; return false; }
        for (const AxisSource* s : {&b.a, &b.b}) {
            if (s == &b.b && b.kind != AxisBinding::Combine) break;
            if (!out.device(s->device)) { err = "[axes] " + kv.first + ": unknown device alias '" + s->device + "'"; return false; }
        }
        used[target] = true;
        out.axes.push_back(b);
    }

    bool btn_used[128] = {};
    for (auto& kv : ini.section("buttons")) {
        if (kv.first == "auto") {
            for (auto& a : split_list(kv.second, ',')) {
                std::string al = Ini::lower(a);
                if (!out.device(al)) { err = "[buttons] auto: unknown device alias '" + a + "'"; return false; }
                out.auto_buttons.push_back(al);
            }
            continue;
        }
        long long t;
        if (!parse_int(kv.first, t) || t < 0 || t > 127) { err = "[buttons] virtual button must be 0..127: " + kv.first; return false; }
        ButtonBinding b;
        b.target = (int)t;
        if (!parse_obj_ref(kv.second, "b", "button", b.device, b.button, err)) { err = "[buttons] " + kv.first + ": " + err; return false; }
        if (!out.device(b.device)) { err = "[buttons] " + kv.first + ": unknown device alias '" + b.device + "'"; return false; }
        if (btn_used[t] && warnings) warnings->push_back("[buttons] " + kv.first + " bound twice; last one wins");
        btn_used[t] = true;
        out.buttons.push_back(b);
    }

    for (auto& kv : ini.section("povs")) {
        long long t;
        if (!parse_int(kv.first, t) || t < 0 || t > 3) { err = "[povs] virtual POV must be 0..3: " + kv.first; return false; }
        PovBinding p;
        p.target = (int)t;
        if (!parse_obj_ref(kv.second, "pov", "hat", p.device, p.pov, err) || p.pov > 3) {
            if (err.empty()) err = "POV index must be 0..3";
            err = "[povs] " + kv.first + ": " + err;
            return false;
        }
        if (!out.device(p.device)) { err = "[povs] " + kv.first + ": unknown device alias '" + p.device + "'"; return false; }
        out.povs.push_back(p);
    }

    if (warnings) {
        for (auto& d : out.devices) {
            bool referenced = d.alias == out.ffb_device;
            for (auto& b : out.axes) referenced |= b.a.device == d.alias || (b.kind == AxisBinding::Combine && b.b.device == d.alias);
            for (auto& b : out.buttons) referenced |= b.device == d.alias;
            for (auto& a : out.auto_buttons) referenced |= a == d.alias;
            for (auto& p : out.povs) referenced |= p.device == d.alias;
            if (!referenced) warnings->push_back("[devices] " + d.alias + " is not used by any mapping");
        }
    }
    return true;
}

static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

double apply_source(const AxisSource& s, double raw01) {
    double n = clamp01((clamp01(raw01) - s.cal_min) / (s.cal_max - s.cal_min));
    if (s.invert) n = 1.0 - n;
    if (s.bipolar) {
        double c = 2.0 * n - 1.0;  // -1..1
        double m = std::fabs(c);
        m = m <= s.deadzone ? 0.0 : (m - s.deadzone) / (1.0 - s.deadzone);
        if (s.gamma != 1.0) m = std::pow(m, s.gamma);
        return 0.5 + 0.5 * (c < 0 ? -m : m);
    }
    n = n <= s.deadzone ? 0.0 : (n - s.deadzone) / (1.0 - s.deadzone);
    if (s.gamma != 1.0) n = std::pow(n, s.gamma);
    return n;
}

double apply_binding(const AxisBinding& b, double raw_a01, double raw_b01) {
    if (b.kind == AxisBinding::Single) return apply_source(b.a, raw_a01);
    // Combined pedals (old GP-style single axis): rest = centre, accelerator up, brake down.
    return clamp01(0.5 + 0.5 * apply_source(b.a, raw_a01) - 0.5 * apply_source(b.b, raw_b01));
}

}  // namespace gp4u
