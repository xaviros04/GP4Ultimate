// GP4's menus at 16:9 at most (centred, black bars) on ultrawide and triple screens.
//
// GP4's menu engine keeps its own screen size in [0x9ed5b8] (width) / [0x9ed5bc] (height).
// When width/height >= 2 it draws into a centred 4:3 viewport (0x5cc820), but some 40 other
// functions keep laying the menu out over the whole width: on 5760x1080 the text comes out
// narrow and misplaced. Below 2 (16:9 included) it simply uses the whole screen, which works.
// So the engine is told a 16:9 width (1920 for 1080 lines): its three writers are patched
//   0x5b73b0  SetScreenSize(w, h) (stdcall, 22 bytes)   -> jmp to our version
//   0x5b47e5  mov [0x9ed5b8], esi  (h in edi)            -> call stub; nop
//   0x5ccc61  mov [0x9ed5b8], ecx  (h in eax)            -> call stub; nop
// and d3d8.dll moves the engine's viewports to the centre of the real back buffer.
// The menu cursor is driven by mouse deltas (0x5c7d5b re-centres the Windows cursor), so
// it needs no mapping.
#pragma once
#include <cmath>
#include <cstdint>
#include <string>

namespace gp4u {

struct MenuSizeTargets {
    uint32_t setter;        // 0x5b73b0
    uint32_t init_store;    // 0x5b47e5
    uint32_t reset_store;   // 0x5ccc61
    uint32_t width;         // int, [0x9ed5b8]
    uint32_t height;        // int, [0x9ed5bc]
};
MenuSizeTargets gp4_menu_size_targets();
extern const uint8_t kMenuSetter[22];
extern const uint8_t kMenuInitStore[6];
extern const uint8_t kMenuResetStore[6];

// The width the menu engine is given: w, or h * max_aspect when the screen is wider
// (rounded down to an even number). max_aspect <= 0: w.
inline int menu_width(int w, int h, double max_aspect) {
    if (!(max_aspect > 0) || w <= 0 || h <= 0) return w;
    double cap = std::floor(h * max_aspect / 2.0) * 2.0;
    return (double)w > cap ? (int)cap : w;
}

// Where a viewport GP4 sets on the back buffer goes. The menu engine's (inside its
// menu_w-wide screen) move to the centre; a full-screen one is narrowed to the centre too
// while the menus are showing (no pre-transformed draws: races are all pre-transformed),
// so the bars stay black. Anything else (the race's full-width view) is left alone.
struct ViewportX { bool moved; uint32_t x, w; };
inline ViewportX menu_viewport(uint32_t x, uint32_t w, uint32_t bb_w, uint32_t menu_w, bool menu_frame) {
    if (menu_w >= bb_w || menu_w == 0) return {false, x, w};
    uint32_t off = (bb_w - menu_w) / 2;
    if (x + w <= menu_w && w < bb_w) return {true, x + off, w};
    if (x == 0 && w == bb_w && menu_frame) return {true, off, menu_w};
    return {false, x, w};
}

bool install_menu_aspect(const MenuSizeTargets& t, double max_aspect, std::string* why);

// For the tests.
void menu_aspect_configure(const MenuSizeTargets& t, double max_aspect);
void* menu_setter_fn();
void* menu_init_stub();
void* menu_reset_stub();

}  // namespace gp4u
