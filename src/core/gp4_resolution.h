// GP4's resolutions (verified by reading GP4.exe, docs/GP4-FINDINGS.md "Resolución"):
// the graphics menu never asks the driver; GP2001Menu_GraphicsSwitchResolution (0x500ec0,
// jump table 0x501130) offers seven fixed 4:3/5:4 sizes, 640x480 .. 1600x1200, and saves
// the choice in f1graphics.cfg ("[RELEASE] Game 3D Horizontal/Vertical Resolution").
// Initialize3DEnvironment (0x5053a0) then needs that exact size in the mode list GP4
// builds with EnumAdapterModes (first 256 raw modes only, 0x504ec5).
//
// [display] resolution = WxH re-targets the menu's 1600x1200 entry to WxH: the immediates
// below (menu, start-up default, menu index lookup, font choice). Pure data + helpers so
// the native tests check the byte edits; the d3d8 proxy applies them in memory.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gp4u {
namespace gp4res {

struct Site {
    uint32_t addr;
    uint8_t op[2];
    int op_len;       // opcode bytes before the 32-bit immediate
    uint32_t orig;    // 1600 or 1200
    bool width;
    const char* what;
};

inline const std::vector<Site>& sites() {
    static const std::vector<Site> s = {
        {0x500fa5, {0xBE, 0}, 1, 1600, true, "menu: mov esi, width"},
        {0x500faa, {0xBF, 0}, 1, 1200, false, "menu: mov edi, height"},
        {0x508d06, {0xBE, 0}, 1, 1600, true, "start-up: mov esi, width"},
        {0x508d0b, {0xBF, 0}, 1, 1200, false, "start-up: mov edi, height"},
        {0x505cbc, {0x81, 0xF9}, 2, 1600, true, "menu index: cmp ecx, width"},
        {0x505cc4, {0x81, 0xFA}, 2, 1200, false, "menu index: cmp edx, height"},
        {0x504cce, {0x3D, 0}, 1, 1600, true, "font: cmp eax, width"},
        {0x504cd5, {0x81, 0xF9}, 2, 1200, false, "font: cmp ecx, height"},
    };
    return s;
}

inline std::vector<uint8_t> bytes(const Site& s, uint32_t imm) {
    std::vector<uint8_t> b(s.op, s.op + s.op_len);
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(imm >> (8 * i)));
    return b;
}

// "1920x1080", "1920 x 1080", "1920*1080" -> true. Sensible bounds only.
inline bool parse_resolution(const std::string& text, int& w, int& h) {
    int a = 0, b = 0;
    char sep = 0;
    if (std::sscanf(text.c_str(), " %d %c %d", &a, &sep, &b) != 3) return false;
    if (sep != 'x' && sep != 'X' && sep != '*') return false;
    if (a < 320 || b < 200 || a > 16384 || b > 8192) return false;
    w = a;
    h = b;
    return true;
}

struct Mode { int w, h; };

// "auto" = the desktop's size (resolved by the d3d8 proxy).
inline bool is_auto(const std::string& text) {
    std::string t;
    for (char c : text)
        if (c != ' ' && c != '\t') t += (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return t == "auto";
}

struct RawMode { int w, h, refresh; unsigned format; };

// Full-screen mode list for GP4. GP4 reads only the first 256 modes the driver lists
// (0x504ec5) and keeps one entry per size and format (it stores refresh 0, i.e. the
// driver's default rate); modern drivers list hundreds of modes (every refresh rate,
// scaling variants), so big or unusual sizes such as 2560x1080 fall past that limit and
// GP4 cannot select them.
//
// needs_fullscreen_list: only then does the mod step in (configured size set and not in
// the first 256 raw modes); otherwise GP4 gets the driver's list untouched.
inline bool needs_fullscreen_list(const std::vector<RawMode>& raw, int cfg_w, int cfg_h) {
    if (cfg_w <= 0 || cfg_h <= 0) return false;
    for (size_t i = 0; i < raw.size() && i < 256; ++i)
        if (raw[i].w == cfg_w && raw[i].h == cfg_h) return false;
    return true;
}

// The real modes once per size and format (the first-listed refresh, as GP4 would keep),
// the configured size first, at most 256.
inline std::vector<RawMode> fullscreen_modes(const std::vector<RawMode>& raw, int cfg_w, int cfg_h) {
    std::vector<RawMode> out;
    for (auto& m : raw) {
        bool found = false;
        for (auto& o : out)
            if (o.w == m.w && o.h == m.h && o.format == m.format) {
                found = true;
                break;
            }
        if (!found) out.push_back(m);
    }
    std::vector<RawMode> sorted;
    for (auto& m : out)
        if (m.w == cfg_w && m.h == cfg_h) sorted.push_back(m);
    for (auto& m : out)
        if (!(m.w == cfg_w && m.h == cfg_h)) sorted.push_back(m);
    if (sorted.size() > 256) sorted.resize(256);
    return sorted;
}

// Sizes the driver offers closest to w x h (for the log when the configured one is missing).
inline std::vector<Mode> closest_sizes(const std::vector<RawMode>& raw, int w, int h, size_t n = 3) {
    std::vector<Mode> sizes;
    for (auto& m : raw) {
        bool dup = false;
        for (auto& s : sizes) dup |= s.w == m.w && s.h == m.h;
        if (!dup) sizes.push_back({m.w, m.h});
    }
    auto dist = [&](const Mode& m) { return (long long)(m.w - w) * (m.w - w) + (long long)(m.h - h) * (m.h - h); };
    for (size_t i = 0; i < sizes.size(); ++i)
        for (size_t j = i + 1; j < sizes.size(); ++j)
            if (dist(sizes[j]) < dist(sizes[i])) std::swap(sizes[i], sizes[j]);
    if (sizes.size() > n) sizes.resize(n);
    return sizes;
}

// Modes offered to GP4 in window mode (no real mode switch happens there, so any size
// the back buffer can take is fine): the configured one first, GP4's own menu sizes,
// common 16:9 sizes and the desktop. Deduplicated, in that order.
inline std::vector<Mode> window_modes(int cfg_w, int cfg_h, int desk_w, int desk_h) {
    std::vector<Mode> out;
    auto add = [&](int w, int h) {
        if (w <= 0 || h <= 0) return;
        for (auto& m : out)
            if (m.w == w && m.h == h) return;
        out.push_back({w, h});
    };
    add(cfg_w, cfg_h);
    for (auto m : {Mode{640, 480}, Mode{800, 600}, Mode{1024, 768}, Mode{1280, 1024}, Mode{1600, 1200},
                   Mode{1280, 720}, Mode{1600, 900}, Mode{1920, 1080}, Mode{2560, 1440}, Mode{3840, 2160}})
        add(m.w, m.h);
    add(desk_w, desk_h);
    return out;
}

}  // namespace gp4res
}  // namespace gp4u
