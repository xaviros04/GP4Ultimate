// GP4 Ultimate d3d8 - d3d8.dll proxy for Grand Prix 4: rendering diagnostics and wide-screen support.
//
//   * frame capture (hotkey, default F11): logs every hooked D3D8 call of the next frames,
//     to learn how GP4 builds a frame (projection via SetTransform or pre-transformed
//     vertices, render passes, cockpit/HUD order). That is the data the triple-screen
//     renderer needs.
//   * aspect diagnostics: logs the aspect the game's projection was built for against the
//     back buffer's, and optionally applies Hor+ correction for wide/Surround/Eyefinity
//     resolutions ([display] aspect_fix = 1).
//   * window mode ([display] window = 1): GP4 always asks for exclusive fullscreen; this
//     turns it into a borderless window of GP4's resolution, centred on the monitor (on a
//     Surround/Eyefinity desktop: the middle screen), and keeps it there.
//   * chaining: [display] chain = d3d8_wrapper.dll loads another d3d8 (dgVoodoo, d3d8to9,
//     ...) for modern-API output; our hooks sit on whatever device it returns.
//
// Hooks are installed on the device vtable, so they also see calls made through objects
// that hand out the device (resource->GetDevice), unlike a COM wrapper.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <string>
#include <vector>

#include "core/gp4_resolution.h"
#include "wide_fov.h"
#include "menu_aspect.h"
#include "core/paths.h"
#include "core/version.h"
#include "core/ini.h"
#include "core/projection.h"
#include "win/log.h"

extern "C" {
extern const int gp4_vt_GetAdapterModeCount, gp4_vt_EnumAdapterModes;
extern const int gp4_vt_CreateDevice, gp4_vt_Reset, gp4_vt_Present, gp4_vt_SetRenderTarget, gp4_vt_BeginScene,
    gp4_vt_EndScene, gp4_vt_Clear, gp4_vt_SetTransform, gp4_vt_MultiplyTransform, gp4_vt_SetViewport,
    gp4_vt_SetRenderState, gp4_vt_SetTexture, gp4_vt_DrawPrimitive, gp4_vt_DrawIndexedPrimitive, gp4_vt_DrawPrimitiveUP,
    gp4_vt_DrawIndexedPrimitiveUP, gp4_vt_SetVertexShader, gp4_vt_SetVertexShaderConstant, gp4_vt_CreateVertexShader;
}

using namespace gp4u;

namespace {

typedef IDirect3D8*(WINAPI* Create8Fn)(UINT);
typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceFn)(IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);
typedef HRESULT(STDMETHODCALLTYPE* ResetFn)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
typedef UINT(STDMETHODCALLTYPE* ModeCountFn)(IDirect3D8*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* EnumModesFn)(IDirect3D8*, UINT, UINT, D3DDISPLAYMODE*);
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* SetRenderTargetFn)(IDirect3DDevice8*, IDirect3DSurface8*, IDirect3DSurface8*);
typedef HRESULT(STDMETHODCALLTYPE* VoidFn)(IDirect3DDevice8*);
typedef HRESULT(STDMETHODCALLTYPE* ClearFn)(IDirect3DDevice8*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* TransformFn)(IDirect3DDevice8*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
typedef HRESULT(STDMETHODCALLTYPE* ViewportFn)(IDirect3DDevice8*, const D3DVIEWPORT8*);
typedef HRESULT(STDMETHODCALLTYPE* RenderStateFn)(IDirect3DDevice8*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* SetTextureFn)(IDirect3DDevice8*, DWORD, IDirect3DBaseTexture8*);
typedef HRESULT(STDMETHODCALLTYPE* DrawFn)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawIdxFn)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawUPFn)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawIdxUPFn)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* SetVSFn)(IDirect3DDevice8*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* SetVSConstFn)(IDirect3DDevice8*, DWORD, const void*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* CreateVSFn)(IDirect3DDevice8*, const DWORD*, const DWORD*, DWORD*, DWORD);

CreateDeviceFn o_CreateDevice;
ModeCountFn o_GetAdapterModeCount;
EnumModesFn o_EnumAdapterModes;
ResetFn o_Reset;
PresentFn o_Present;
SetRenderTargetFn o_SetRenderTarget;
VoidFn o_BeginScene, o_EndScene;
ClearFn o_Clear;
TransformFn o_SetTransform, o_MultiplyTransform;
ViewportFn o_SetViewport;
RenderStateFn o_SetRenderState;
SetTextureFn o_SetTexture;
DrawFn o_DrawPrimitive;
DrawIdxFn o_DrawIndexedPrimitive;
DrawUPFn o_DrawPrimitiveUP;
DrawIdxUPFn o_DrawIndexedPrimitiveUP;
SetVSFn o_SetVertexShader;
SetVSConstFn o_SetVertexShaderConstant;
CreateVSFn o_CreateVertexShader;

struct Settings {
    std::string dir;
    Ini ini;
    bool aspect_fix = false;
    bool wide_fov = true;       // [display] wide_fov: Hor+ in GP4's own camera (wide_fov.h)
    double fov_ref_aspect = 16.0 / 9.0;
    double fov_zoom = 1.0;      // [display] fov_zoom: > 1 = camera closer
    double menu_aspect = 16.0 / 9.0;   // [display] menu_aspect: widest menus (0 = GP4's own)
    double aspect = 0;          // 0 = back buffer
    double fov_scale = 1.0;
    int capture_key = VK_F11;
    int capture_frames = 2;
    bool window = false;
    int res_w = 0, res_h = 0;   // [display] resolution: replaces the menu's 1600x1200
    bool res_auto = false;      // resolution = auto (the desktop's)
    bool window_x_auto = true, window_y_auto = true;
    int window_x = 0, window_y = 0;
} g_set;

// Window mode state.
IDirect3D8* g_d3d = nullptr;
UINT g_adapter = 0;
HWND g_win = nullptr;
WNDPROC g_old_proc = nullptr;
RECT g_win_rect{};
bool g_win_unicode = false;

std::mutex g_mtx;   // GP4 renders from one thread; this only guards the capture file
UINT g_bb_w = 0, g_bb_h = 0;
HWND g_hwnd = nullptr;
bool g_logged_aspect = false;

// [display] menu_aspect: GP4's menu engine thinks the screen is g_menu_w wide; its viewports
// are moved to the centre of the real back buffer (menu_aspect.h).
bool g_menu_on = false;
bool g_rt_bb = true;              // drawing into the back buffer (not a texture)
unsigned g_rhw_bb = 0;            // pre-transformed draws into the back buffer this frame
bool g_menu_frame = true;         // last frame had none: menus (races are all pre-transformed)
int menu_w() { return g_menu_on ? menu_width((int)g_bb_w, (int)g_bb_h, g_set.menu_aspect) : (int)g_bb_w; }

// Capture state.
FILE* g_cap = nullptr;
int g_cap_left = 0, g_cap_index = 0;
unsigned long g_frame = 0;
DWORD g_vs = 0;   // current FVF or shader handle
struct FrameStats {
    unsigned draws = 0, pretransformed = 0, shader_draws = 0, up_draws = 0, prims = 0;
    unsigned proj_sets = 0, view_sets = 0, world_sets = 0, viewports = 0, rt_sets = 0, scenes = 0, clears = 0;
} g_fs;

void cap(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void cap(const char* fmt, ...) {
    if (!g_cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_cap, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_cap);
}

const char* ts_name(D3DTRANSFORMSTATETYPE t) {
    switch ((int)t) {
        case D3DTS_VIEW: return "VIEW";
        case D3DTS_PROJECTION: return "PROJECTION";
        case D3DTS_TEXTURE0: return "TEXTURE0";
        case 256: return "WORLD";
        default: return "OTHER";
    }
}

std::string fvf_str(DWORD v) {
    if (v & 1) {
        char b[32];
        std::snprintf(b, sizeof b, "shader#%08lx", (unsigned long)v);
        return b;
    }
    std::string s;
    switch (v & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZ: s = "XYZ"; break;
        case D3DFVF_XYZRHW: s = "XYZRHW(pre-transformed)"; break;
        case D3DFVF_XYZB1: s = "XYZB1"; break;
        case D3DFVF_XYZB2: s = "XYZB2"; break;
        case D3DFVF_XYZB3: s = "XYZB3"; break;
        case D3DFVF_XYZB4: s = "XYZB4"; break;
        case D3DFVF_XYZB5: s = "XYZB5"; break;
        default: s = "none";
    }
    if (v & D3DFVF_NORMAL) s += "|N";
    if (v & D3DFVF_DIFFUSE) s += "|DIFF";
    if (v & D3DFVF_SPECULAR) s += "|SPEC";
    char b[24];
    std::snprintf(b, sizeof b, "|TEX%lu", (unsigned long)((v & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT));
    return s + b;
}

void note_draw(const char* kind, D3DPRIMITIVETYPE t, UINT prims) {
    ++g_fs.draws;
    g_fs.prims += prims;
    if (g_vs & 1) ++g_fs.shader_draws;
    else if ((g_vs & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
        ++g_fs.pretransformed;
        if (g_rt_bb) ++g_rhw_bb;
    }
    if (g_cap) cap("  %s type=%d prims=%u vs=%s", kind, (int)t, prims, fvf_str(g_vs).c_str());
}

void hook(void** vtbl, int idx, void* fn, void** orig) {
    if (vtbl[idx] == fn) return;   // already hooked (shared vtable)
    DWORD old;
    VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = vtbl[idx];
    vtbl[idx] = fn;
    VirtualProtect(&vtbl[idx], sizeof(void*), old, &old);
}

void update_backbuffer(const D3DPRESENT_PARAMETERS* pp, HWND focus) {
    g_bb_w = pp->BackBufferWidth;
    g_bb_h = pp->BackBufferHeight;
    g_rt_bb = true;
    g_hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    if ((!g_bb_w || !g_bb_h) && g_hwnd) {
        RECT r;
        if (GetClientRect(g_hwnd, &r)) { g_bb_w = r.right - r.left; g_bb_h = r.bottom - r.top; }
    }
    g_logged_aspect = false;
    logf("back buffer %ux%u %s, format %d, refresh %u Hz, swap effect %d", g_bb_w, g_bb_h,
         pp->Windowed ? "windowed" : "fullscreen", (int)pp->BackBufferFormat, pp->FullScreen_RefreshRateInHz,
         (int)pp->SwapEffect);
}

double target_aspect() {
    if (g_set.aspect > 0) return g_set.aspect;
    return (g_bb_w && g_bb_h) ? (double)g_bb_w / g_bb_h : 0;
}

// ------------------------------------------------------------------ device hooks
HRESULT STDMETHODCALLTYPE h_SetTransform(IDirect3DDevice8* d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m) {
    if (!m) return o_SetTransform(d, t, m);
    if (t == D3DTS_PROJECTION) {
        ++g_fs.proj_sets;
        Mat4 p;
        std::memcpy(&p, m, sizeof p);
        double ta = target_aspect();
        bool persp = is_perspective(p);
        if (!g_logged_aspect && persp && ta > 0) {
            g_logged_aspect = true;
            double ga = projection_aspect(p);
            logf("projection: game builds aspect %.3f (hfov %.1f deg), back buffer aspect %.3f -> %s", ga,
                 horizontal_fov_deg(p), ta,
                 std::fabs(ga - ta) < 0.01 ? "matches" : (g_set.aspect_fix ? "correcting (Hor+)" : "STRETCHED (set [display] aspect_fix = 1)"));
        }
        if (g_cap) cap("  SetTransform PROJECTION %s _11=%.4f _22=%.4f _33=%.4f _34=%.1f _43=%.4f _31=%.4f _32=%.4f",
                       persp ? "perspective" : "non-perspective", p.m[0][0], p.m[1][1], p.m[2][2], p.m[2][3], p.m[3][2],
                       p.m[2][0], p.m[2][1]);
        if (g_set.aspect_fix && persp && ta > 0 && fix_aspect(p, ta, g_set.fov_scale)) {
            D3DMATRIX fixed;
            std::memcpy(&fixed, &p, sizeof fixed);
            return o_SetTransform(d, t, &fixed);
        }
    } else if (g_cap) {
        if (t == D3DTS_VIEW) ++g_fs.view_sets;
        else if ((int)t == 256) ++g_fs.world_sets;
        cap("  SetTransform %s [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | t %.2f %.2f %.2f]", ts_name(t),
            m->_11, m->_12, m->_13, m->_21, m->_22, m->_23, m->_31, m->_32, m->_33, m->_41, m->_42, m->_43);
    } else {
        if (t == D3DTS_VIEW) ++g_fs.view_sets;
        else if ((int)t == 256) ++g_fs.world_sets;
    }
    return o_SetTransform(d, t, m);
}

HRESULT STDMETHODCALLTYPE h_MultiplyTransform(IDirect3DDevice8* d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m) {
    if (g_cap) cap("  MultiplyTransform %s", ts_name(t));
    return o_MultiplyTransform(d, t, m);
}

HRESULT STDMETHODCALLTYPE h_SetViewport(IDirect3DDevice8* d, const D3DVIEWPORT8* v) {
    ++g_fs.viewports;
    if (g_cap && v) cap("  SetViewport x=%lu y=%lu w=%lu h=%lu z=%.2f..%.2f", (unsigned long)v->X, (unsigned long)v->Y,
                        (unsigned long)v->Width, (unsigned long)v->Height, v->MinZ, v->MaxZ);
    if (v && g_menu_on && g_rt_bb) {
        ViewportX mx = menu_viewport(v->X, v->Width, g_bb_w, (uint32_t)menu_w(), g_menu_frame);
        if (mx.moved) {
            D3DVIEWPORT8 m = *v;
            m.X = mx.x;
            m.Width = mx.w;
            if (g_cap) cap("    -> x=%lu w=%lu (menu, centred)", (unsigned long)m.X, (unsigned long)m.Width);
            return o_SetViewport(d, &m);
        }
    }
    return o_SetViewport(d, v);
}

HRESULT STDMETHODCALLTYPE h_SetRenderTarget(IDirect3DDevice8* d, IDirect3DSurface8* rt, IDirect3DSurface8* z) {
    ++g_fs.rt_sets;
    D3DSURFACE_DESC desc{};
    if (rt) {
        rt->GetDesc(&desc);
        g_rt_bb = desc.Width == g_bb_w && desc.Height == g_bb_h;
    }
    if (g_cap) cap("  SetRenderTarget rt=%p (%ux%u fmt %d) z=%p", (void*)rt, desc.Width, desc.Height, (int)desc.Format, (void*)z);
    return o_SetRenderTarget(d, rt, z);
}

HRESULT STDMETHODCALLTYPE h_BeginScene(IDirect3DDevice8* d) {
    ++g_fs.scenes;
    if (g_cap) cap("  BeginScene");
    return o_BeginScene(d);
}

HRESULT STDMETHODCALLTYPE h_EndScene(IDirect3DDevice8* d) {
    if (g_cap) cap("  EndScene");
    return o_EndScene(d);
}

HRESULT STDMETHODCALLTYPE h_Clear(IDirect3DDevice8* d, DWORD n, const D3DRECT* r, DWORD f, D3DCOLOR c, float z, DWORD s) {
    ++g_fs.clears;
    if (g_cap) cap("  Clear rects=%lu flags=0x%lx color=%08lx z=%.2f", (unsigned long)n, (unsigned long)f, (unsigned long)c, z);
    // Menus: the bars beside the centred menu stay black (and lose the last race image).
    if (g_menu_on && g_menu_frame && g_rt_bb && n == 0 && (f & D3DCLEAR_TARGET) && menu_w() < (int)g_bb_w) {
        D3DVIEWPORT8 vp{}, full{0, 0, g_bb_w, g_bb_h, 0.0f, 1.0f};
        d->GetViewport(&vp);
        o_SetViewport(d, &full);
        o_Clear(d, 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        o_SetViewport(d, &vp);
    }
    return o_Clear(d, n, r, f, c, z, s);
}

HRESULT STDMETHODCALLTYPE h_SetRenderState(IDirect3DDevice8* d, D3DRENDERSTATETYPE s, DWORD v) {
    if (g_cap) cap("  SetRenderState %d = %lu", (int)s, (unsigned long)v);
    return o_SetRenderState(d, s, v);
}

HRESULT STDMETHODCALLTYPE h_SetTexture(IDirect3DDevice8* d, DWORD stage, IDirect3DBaseTexture8* t) {
    if (g_cap) cap("  SetTexture %lu %p", (unsigned long)stage, (void*)t);
    return o_SetTexture(d, stage, t);
}

HRESULT STDMETHODCALLTYPE h_SetVertexShader(IDirect3DDevice8* d, DWORD h) {
    g_vs = h;
    if (g_cap) cap("  SetVertexShader %s", fvf_str(h).c_str());
    return o_SetVertexShader(d, h);
}

HRESULT STDMETHODCALLTYPE h_SetVertexShaderConstant(IDirect3DDevice8* d, DWORD reg, const void* data, DWORD count) {
    if (g_cap && data) {
        const float* f = (const float*)data;
        cap("  SetVertexShaderConstant c%lu x%lu  first=[%.3f %.3f %.3f %.3f]", (unsigned long)reg, (unsigned long)count,
            f[0], f[1], f[2], f[3]);
    }
    return o_SetVertexShaderConstant(d, reg, data, count);
}

HRESULT STDMETHODCALLTYPE h_CreateVertexShader(IDirect3DDevice8* d, const DWORD* decl, const DWORD* code, DWORD* h, DWORD usage) {
    HRESULT hr = o_CreateVertexShader(d, decl, code, h, usage);
    logf("CreateVertexShader %s -> handle %08lx (%s)", code ? "programmable" : "fixed-function decl",
         h ? (unsigned long)*h : 0ul, SUCCEEDED(hr) ? "ok" : "failed");
    return hr;
}

HRESULT STDMETHODCALLTYPE h_DrawPrimitive(IDirect3DDevice8* d, D3DPRIMITIVETYPE t, UINT start, UINT n) {
    note_draw("DrawPrimitive", t, n);
    return o_DrawPrimitive(d, t, start, n);
}

HRESULT STDMETHODCALLTYPE h_DrawIndexedPrimitive(IDirect3DDevice8* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT si, UINT n) {
    note_draw("DrawIndexedPrimitive", t, n);
    return o_DrawIndexedPrimitive(d, t, mi, nv, si, n);
}

HRESULT STDMETHODCALLTYPE h_DrawPrimitiveUP(IDirect3DDevice8* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride) {
    ++g_fs.up_draws;
    note_draw("DrawPrimitiveUP", t, n);
    return o_DrawPrimitiveUP(d, t, n, v, stride);
}

HRESULT STDMETHODCALLTYPE h_DrawIndexedPrimitiveUP(IDirect3DDevice8* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT n,
                                                   const void* idx, D3DFORMAT f, const void* v, UINT stride) {
    ++g_fs.up_draws;
    note_draw("DrawIndexedPrimitiveUP", t, n);
    return o_DrawIndexedPrimitiveUP(d, t, mi, nv, n, idx, f, v, stride);
}

void end_frame_stats() {
    if (g_cap) {
        cap("=== frame %lu: %u draws (%u pre-transformed XYZRHW, %u vertex-shader, %u UP), %u primitives,"
            " PROJECTION x%u VIEW x%u WORLD x%u, viewports %u, render targets %u, scenes %u, clears %u",
            g_frame, g_fs.draws, g_fs.pretransformed, g_fs.shader_draws, g_fs.up_draws, g_fs.prims, g_fs.proj_sets,
            g_fs.view_sets, g_fs.world_sets, g_fs.viewports, g_fs.rt_sets, g_fs.scenes, g_fs.clears);
        logf("capture frame %lu: %u draws, %u pre-transformed, %u shader, PROJECTION x%u", g_frame, g_fs.draws,
             g_fs.pretransformed, g_fs.shader_draws, g_fs.proj_sets);
        if (--g_cap_left <= 0) {
            std::fclose(g_cap);
            g_cap = nullptr;
            logf("capture %d written", g_cap_index);
        }
    }
    g_fs = FrameStats();
}

HRESULT STDMETHODCALLTYPE h_Present(IDirect3DDevice8* d, const RECT* s, const RECT* dr, HWND w, const RGNDATA* r) {
    HRESULT hr = o_Present(d, s, dr, w, r);
    g_menu_frame = g_rhw_bb == 0;
    g_rhw_bb = 0;
    std::lock_guard<std::mutex> lk(g_mtx);
    end_frame_stats();
    ++g_frame;
    if (!g_cap && (GetAsyncKeyState(g_set.capture_key) & 1)) {
        char name[64];
        std::snprintf(name, sizeof name, "\\gp4ultimate_d3d_capture_%d.txt", ++g_cap_index);
        g_cap = std::fopen((g_set.dir + name).c_str(), "w");
        g_cap_left = g_set.capture_frames;
        logf("capture %d started at frame %lu (%d frames)", g_cap_index, g_frame, g_cap_left);
        cap("# GP4 Ultimate d3d8 frame capture %d - back buffer %ux%u, aspect_fix=%d", g_cap_index, g_bb_w, g_bb_h, g_set.aspect_fix);
        cap("# current vertex shader/FVF at start: %s", fvf_str(g_vs).c_str());
    }
    return hr;
}

// GP4 (or its fullscreen code) may move/resize its window later; keep ours.
LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_WINDOWPOSCHANGING && g_win_rect.right > g_win_rect.left) {
        auto* p = (WINDOWPOS*)lp;
        bool minimising = p->x <= -30000 || p->y <= -30000 || IsIconic(h);
        if (!minimising) {
            if (!(p->flags & SWP_NOMOVE)) { p->x = g_win_rect.left; p->y = g_win_rect.top; }
            if (!(p->flags & SWP_NOSIZE)) {
                p->cx = g_win_rect.right - g_win_rect.left;
                p->cy = g_win_rect.bottom - g_win_rect.top;
            }
        }
    }
    return g_win_unicode ? CallWindowProcW(g_old_proc, h, msg, wp, lp) : CallWindowProcA(g_old_proc, h, msg, wp, lp);
}

void place_window(HWND hwnd, UINT w, UINT h) {
    if (!hwnd || !IsWindow(hwnd) || !w || !h) return;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& m = mi.rcMonitor;
    int x = g_set.window_x_auto ? m.left + ((m.right - m.left) - (int)w) / 2 : g_set.window_x;
    int y = g_set.window_y_auto ? m.top + ((m.bottom - m.top) - (int)h) / 2 : g_set.window_y;
    g_win_rect = {x, y, x + (int)w, y + (int)h};
    LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME);
    SetWindowLongA(hwnd, GWL_STYLE, style | WS_POPUP | WS_VISIBLE);
    LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
    SetWindowLongA(hwnd, GWL_EXSTYLE, ex & ~(WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
    if (g_win != hwnd) {
        g_win = hwnd;
        g_win_unicode = IsWindowUnicode(hwnd) != FALSE;
        g_old_proc = g_win_unicode ? (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)window_proc)
                                   : (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)window_proc);
    }
    SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, (int)w, (int)h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    logf("window mode: %ux%u at (%d,%d) on a %ldx%ld monitor at (%ld,%ld)", w, h, x, y, m.right - m.left,
         m.bottom - m.top, m.left, m.top);
}

// Turn GP4's exclusive-fullscreen request into a windowed one (D3D8 rules: the back buffer
// must use the desktop's format, no refresh rate, default presentation interval).
void make_windowed(D3DPRESENT_PARAMETERS* pp, HWND focus) {
    if (!g_set.window || !pp) return;
    bool was_full = !pp->Windowed;
    D3DDISPLAYMODE dm{};
    if (g_d3d && SUCCEEDED(g_d3d->GetAdapterDisplayMode(g_adapter, &dm))) pp->BackBufferFormat = dm.Format;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->FullScreen_PresentationInterval = 0;
    if (pp->SwapEffect == D3DSWAPEFFECT_COPY_VSYNC) pp->SwapEffect = D3DSWAPEFFECT_COPY;
    if (was_full) logf("window mode: GP4 asked for fullscreen %ux%u, using a window", pp->BackBufferWidth, pp->BackBufferHeight);
    HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    place_window(w, pp->BackBufferWidth, pp->BackBufferHeight);
}

HRESULT STDMETHODCALLTYPE h_Reset(IDirect3DDevice8* d, D3DPRESENT_PARAMETERS* pp) {
    make_windowed(pp, g_hwnd);
    HRESULT hr = o_Reset(d, pp);
    if (SUCCEEDED(hr) && pp) update_backbuffer(pp, g_hwnd);
    logf("Reset -> 0x%08lx", (unsigned long)hr);
    return hr;
}

void hook_device(IDirect3DDevice8* dev) {
    void** v = *(void***)dev;
    hook(v, gp4_vt_Reset, (void*)h_Reset, (void**)&o_Reset);
    hook(v, gp4_vt_Present, (void*)h_Present, (void**)&o_Present);
    hook(v, gp4_vt_SetRenderTarget, (void*)h_SetRenderTarget, (void**)&o_SetRenderTarget);
    hook(v, gp4_vt_BeginScene, (void*)h_BeginScene, (void**)&o_BeginScene);
    hook(v, gp4_vt_EndScene, (void*)h_EndScene, (void**)&o_EndScene);
    hook(v, gp4_vt_Clear, (void*)h_Clear, (void**)&o_Clear);
    hook(v, gp4_vt_SetTransform, (void*)h_SetTransform, (void**)&o_SetTransform);
    hook(v, gp4_vt_MultiplyTransform, (void*)h_MultiplyTransform, (void**)&o_MultiplyTransform);
    hook(v, gp4_vt_SetViewport, (void*)h_SetViewport, (void**)&o_SetViewport);
    hook(v, gp4_vt_SetRenderState, (void*)h_SetRenderState, (void**)&o_SetRenderState);
    hook(v, gp4_vt_SetTexture, (void*)h_SetTexture, (void**)&o_SetTexture);
    hook(v, gp4_vt_DrawPrimitive, (void*)h_DrawPrimitive, (void**)&o_DrawPrimitive);
    hook(v, gp4_vt_DrawIndexedPrimitive, (void*)h_DrawIndexedPrimitive, (void**)&o_DrawIndexedPrimitive);
    hook(v, gp4_vt_DrawPrimitiveUP, (void*)h_DrawPrimitiveUP, (void**)&o_DrawPrimitiveUP);
    hook(v, gp4_vt_DrawIndexedPrimitiveUP, (void*)h_DrawIndexedPrimitiveUP, (void**)&o_DrawIndexedPrimitiveUP);
    hook(v, gp4_vt_SetVertexShader, (void*)h_SetVertexShader, (void**)&o_SetVertexShader);
    hook(v, gp4_vt_SetVertexShaderConstant, (void*)h_SetVertexShaderConstant, (void**)&o_SetVertexShaderConstant);
    hook(v, gp4_vt_CreateVertexShader, (void*)h_CreateVertexShader, (void**)&o_CreateVertexShader);
}

// Mode lists GP4 reads at start-up (first 256 entries only, 0x504ec5), per adapter.
//  - Window mode: no real mode switch happens, so GP4 gets a short synthetic list that is
//    sure to contain its configured size (the Surround driver's list may lack it).
//  - Full screen: the driver's own list, unless the configured size is past the first 256
//    modes; then the real modes once per size and format, the configured size first
//    (core/gp4_resolution.h). Without [display] resolution, always the driver's list.
struct AdapterModes {
    bool synthetic = false;                 // false: pass the driver's list through
    std::vector<gp4res::Mode> window;       // window mode (x2: 32 and 16 bit)
    std::vector<gp4res::RawMode> full;      // full screen
};
std::map<UINT, AdapterModes> g_modes;
const D3DFORMAT kModeFormats[2] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5};

AdapterModes& modes_for(IDirect3D8* d3d, UINT adapter) {
    auto it = g_modes.find(adapter);
    if (it != g_modes.end()) return it->second;
    AdapterModes& am = g_modes[adapter];
    D3DDISPLAYMODE dm{};
    d3d->GetAdapterDisplayMode(adapter, &dm);
    if (g_set.res_auto && g_set.res_w && ((int)dm.Width != g_set.res_w || (int)dm.Height != g_set.res_h))
        logf("resolution = auto gave %dx%d, but adapter %u's desktop is %ux%u: set the size explicitly if GP4 does "
             "not find it", g_set.res_w, g_set.res_h, adapter, dm.Width, dm.Height);
    if (g_set.window) {
        am.synthetic = true;
        am.window = gp4res::window_modes(g_set.res_w, g_set.res_h, (int)dm.Width, (int)dm.Height);
        std::string list;
        for (auto& m : am.window) list += " " + std::to_string(m.w) + "x" + std::to_string(m.h);
        logf("window mode: offering GP4 these modes on adapter %u (32 and 16 bit):%s", adapter, list.c_str());
        return am;
    }
    std::vector<gp4res::RawMode> raw;
    UINT n = o_GetAdapterModeCount(d3d, adapter);
    for (UINT i = 0; i < n; ++i) {
        D3DDISPLAYMODE m{};
        if (SUCCEEDED(o_EnumAdapterModes(d3d, adapter, i, &m)))
            raw.push_back({(int)m.Width, (int)m.Height, (int)m.RefreshRate, (unsigned)m.Format});
    }
    if (!g_set.res_w) return am;
    bool listed = false;
    for (auto& m : raw) listed |= m.w == g_set.res_w && m.h == g_set.res_h;
    if (!listed) {
        std::string closest;
        for (auto& m : gp4res::closest_sizes(raw, g_set.res_w, g_set.res_h))
            closest += " " + std::to_string(m.w) + "x" + std::to_string(m.h);
        logf("full screen: adapter %u has no %dx%d mode (closest:%s). Use one of those, or [display] window = 1, "
             "which works with any size", adapter, g_set.res_w, g_set.res_h, closest.c_str());
    } else if (gp4res::needs_fullscreen_list(raw, g_set.res_w, g_set.res_h)) {
        am.synthetic = true;
        am.full = gp4res::fullscreen_modes(raw, g_set.res_w, g_set.res_h);
        logf("full screen: adapter %u lists %u modes and %dx%d is past the 256 GP4 reads; GP4 gets %u (one per size "
             "and format, the configured size first)", adapter, n, g_set.res_w, g_set.res_h, (unsigned)am.full.size());
    } else {
        logf("full screen: adapter %u lists %dx%d within the first 256 modes; driver's list unchanged", adapter,
             g_set.res_w, g_set.res_h);
    }
    return am;
}

UINT STDMETHODCALLTYPE h_GetAdapterModeCount(IDirect3D8* d3d, UINT adapter) {
    AdapterModes& am = modes_for(d3d, adapter);
    if (!am.synthetic) return o_GetAdapterModeCount(d3d, adapter);
    return g_set.window ? (UINT)(am.window.size() * 2) : (UINT)am.full.size();
}

HRESULT STDMETHODCALLTYPE h_EnumAdapterModes(IDirect3D8* d3d, UINT adapter, UINT i, D3DDISPLAYMODE* m) {
    AdapterModes& am = modes_for(d3d, adapter);
    if (!am.synthetic) return o_EnumAdapterModes(d3d, adapter, i, m);
    if (g_set.window) {
        if (!m || i >= am.window.size() * 2) return D3DERR_INVALIDCALL;
        m->Width = (UINT)am.window[i / 2].w;
        m->Height = (UINT)am.window[i / 2].h;
        m->RefreshRate = 60;
        m->Format = kModeFormats[i % 2];
        return D3D_OK;
    }
    if (!m || i >= am.full.size()) return D3DERR_INVALIDCALL;
    const auto& f = am.full[i];
    m->Width = (UINT)f.w;
    m->Height = (UINT)f.h;
    m->RefreshRate = (UINT)f.refresh;
    m->Format = (D3DFORMAT)f.format;
    return D3D_OK;
}

// [display] resolution: GP4's graphics menu has seven fixed sizes; re-target its
// 1600x1200 entry (code immediates, see core/gp4_resolution.h). Only on the known build.
void patch_resolution() {
    if (!g_set.res_w) return;
    const auto& sites = gp4res::sites();
    for (auto& s : sites) {
        auto want = gp4res::bytes(s, s.orig);
        auto done = gp4res::bytes(s, s.width ? (uint32_t)g_set.res_w : (uint32_t)g_set.res_h);
        std::vector<uint8_t> cur(want.size());
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)s.addr, cur.data(), cur.size(), &got) ||
            got != cur.size() || (cur != want && cur != done)) {
            logf("resolution: GP4.exe not recognised at 0x%08x (%s) - menu not changed", s.addr, s.what);
            return;
        }
    }
    for (auto& s : sites) {
        auto b = gp4res::bytes(s, s.width ? (uint32_t)g_set.res_w : (uint32_t)g_set.res_h);
        DWORD old;
        VirtualProtect((LPVOID)(uintptr_t)s.addr, b.size(), PAGE_EXECUTE_READWRITE, &old);
        std::memcpy((void*)(uintptr_t)s.addr, b.data(), b.size());
        VirtualProtect((LPVOID)(uintptr_t)s.addr, b.size(), old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    logf("resolution: the menu's \"1600x1200\" now means %dx%d (choose it once in GP4's graphics options)",
         g_set.res_w, g_set.res_h);
}

HRESULT STDMETHODCALLTYPE h_CreateDevice(IDirect3D8* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                         D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** out) {
    g_d3d = d3d;
    g_adapter = adapter;
    make_windowed(pp, focus);
    HRESULT hr = o_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    D3DADAPTER_IDENTIFIER8 id{};
    d3d->GetAdapterIdentifier(adapter, 0, &id);
    logf("CreateDevice adapter %u \"%s\" type %d behaviour 0x%lx -> 0x%08lx", adapter, id.Description, (int)type,
         (unsigned long)flags, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) {
        if (pp) update_backbuffer(pp, focus);
        hook_device(*out);
        logf("device hooks installed (capture key 0x%02x)", g_set.capture_key);
    }
    return hr;
}

void load_settings(HMODULE self) {
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    g_set.dir = path;
    g_set.dir = g_set.dir.substr(0, g_set.dir.find_last_of("\\/"));
    g_set.ini.load_file(config_file(g_set.dir));
    if (g_set.ini.get_bool("display", "log", true)) log_open(g_set.dir + "\\gp4ultimate_d3d.log");
    g_set.aspect_fix = g_set.ini.get_bool("display", "aspect_fix", false);
    g_set.fov_scale = g_set.ini.get_double("display", "fov_scale", 1.0);
    g_set.wide_fov = g_set.ini.get_bool("display", "wide_fov", true);
    auto ratio = [](const char* key, double& out) {
        std::string r = Ini::lower(g_set.ini.get("display", key, ""));
        if (r.empty()) return;
        if (r == "off" || r == "0" || r == "none") { out = 0; return; }
        size_t c = r.find(':');
        double rw, rh;
        if (c != std::string::npos && parse_double(r.substr(0, c), rw) && parse_double(r.substr(c + 1), rh) && rh > 0 && rw / rh >= 1.0)
            out = rw / rh;
        else if (parse_double(r, rw) && rw >= 1.0)
            out = rw;
        else
            logf("%s = %s not understood (use e.g. 16:9) - using %.3f", key, r.c_str(), out);
    };
    ratio("fov_ref_aspect", g_set.fov_ref_aspect);
    ratio("menu_aspect", g_set.menu_aspect);
    g_set.fov_zoom = std::clamp(g_set.ini.get_double("display", "fov_zoom", 1.0), 0.3, 4.0);
    std::string a = Ini::lower(g_set.ini.get("display", "aspect", "auto"));
    size_t colon = a.find(':');
    double w, h;
    if (colon != std::string::npos && parse_double(a.substr(0, colon), w) && parse_double(a.substr(colon + 1), h) && h > 0)
        g_set.aspect = w / h;
    else if (parse_double(a, w))
        g_set.aspect = w;
    g_set.capture_key = (int)g_set.ini.get_int("display", "capture_key", VK_F11);
    g_set.capture_frames = (int)std::max<long long>(1, g_set.ini.get_int("display", "capture_frames", 2));
    g_set.window = g_set.ini.get_bool("display", "window", false);
    std::string wx = Ini::lower(g_set.ini.get("display", "window_x", "auto"));
    std::string wy = Ini::lower(g_set.ini.get("display", "window_y", "auto"));
    std::string res = g_set.ini.get("display", "resolution", "");
    if (gp4res::is_auto(res)) {
        g_set.res_auto = true;
        DEVMODEA dm{};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
            g_set.res_w = (int)dm.dmPelsWidth;
            g_set.res_h = (int)dm.dmPelsHeight;
            logf("resolution = auto: the desktop's, %dx%d", g_set.res_w, g_set.res_h);
        } else {
            logf("resolution = auto: could not read the desktop's size - GP4's menu unchanged");
        }
    } else if (!res.empty() && !gp4res::parse_resolution(res, g_set.res_w, g_set.res_h)) {
        logf("resolution = %s not understood (use e.g. 1920x1080 or auto)", res.c_str());
    }
    double v;
    if (wx != "auto" && parse_double(wx, v)) { g_set.window_x_auto = false; g_set.window_x = (int)v; }
    if (wy != "auto" && parse_double(wy, v)) { g_set.window_y_auto = false; g_set.window_y = (int)v; }
    logf("GP4 Ultimate %s (d3d8.dll) loaded: aspect_fix=%d aspect=%s fov_scale=%.2f window=%d", kVersion, g_set.aspect_fix ? 1 : 0, a.c_str(),
         g_set.fov_scale, g_set.window ? 1 : 0);
}

HMODULE g_self = nullptr;
std::once_flag g_once;
Create8Fn g_real = nullptr;
HMODULE g_real_mod = nullptr;

void init() {
    load_settings(g_self);
    std::string chain = g_set.ini.get("display", "chain", "");
    HMODULE real = nullptr;
    if (!chain.empty()) {
        std::string p = chain.find(':') == std::string::npos ? g_set.dir + "\\" + chain : chain;
        real = LoadLibraryA(p.c_str());
        logf("chain: %s %s", p.c_str(), real ? "loaded" : "NOT FOUND - using system d3d8");
    }
    if (!real) {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        lstrcatA(sys, "\\d3d8.dll");
        real = LoadLibraryA(sys);
    }
    g_real_mod = real;
    g_real = real ? (Create8Fn)GetProcAddress(real, "Direct3DCreate8") : nullptr;
    if (!g_real) logf("FATAL: no real Direct3DCreate8");
    patch_resolution();
    if (g_set.wide_fov) {
        std::string why;
        if (install_wide_fov(gp4_wide_fov_targets(), g_set.fov_ref_aspect, &why, g_set.fov_zoom))
            logf("wide_fov: screens wider than %.3f see more to the sides instead of less above and below (Hor+), zoom %.2f",
                 g_set.fov_ref_aspect, g_set.fov_zoom);
        else
            logf("wide_fov: not installed: %s", why.c_str());
    }
    if (g_set.menu_aspect > 0) {
        std::string why;
        if (install_menu_aspect(gp4_menu_size_targets(), g_set.menu_aspect, &why))
            logf("menu_aspect: menus at most %.3f wide, centred", g_set.menu_aspect);
        else
            logf("menu_aspect: not installed: %s", why.c_str());
        g_menu_on = why.empty();
    }
}

}  // namespace

extern "C" IDirect3D8* WINAPI Direct3DCreate8(UINT sdk) {
    std::call_once(g_once, init);
    if (!g_real) return nullptr;
    IDirect3D8* d3d = g_real(sdk);
    if (d3d) {
        hook(*(void***)d3d, gp4_vt_CreateDevice, (void*)h_CreateDevice, (void**)&o_CreateDevice);
        hook(*(void***)d3d, gp4_vt_GetAdapterModeCount, (void*)h_GetAdapterModeCount, (void**)&o_GetAdapterModeCount);
        hook(*(void***)d3d, gp4_vt_EnumAdapterModes, (void*)h_EnumAdapterModes, (void**)&o_EnumAdapterModes);
        logf("Direct3DCreate8(%u) -> hooked", sdk);
    }
    return d3d;
}

// The other exports of the system d3d8.dll, forwarded in case GP4 imports any of them
// (a missing import would stop the game from starting at all).
template <typename F>
static F real_fn(const char* name) {
    std::call_once(g_once, init);
    return g_real_mod ? (F)GetProcAddress(g_real_mod, name) : nullptr;
}

extern "C" HRESULT WINAPI ValidatePixelShader(const DWORD* code, const D3DCAPS8* caps, BOOL errors, char** out) {
    auto f = real_fn<HRESULT(WINAPI*)(const DWORD*, const D3DCAPS8*, BOOL, char**)>("ValidatePixelShader");
    return f ? f(code, caps, errors, out) : E_FAIL;
}

extern "C" HRESULT WINAPI ValidateVertexShader(const DWORD* code, const DWORD* r1, const DWORD* r2, BOOL errors, char** out) {
    auto f = real_fn<HRESULT(WINAPI*)(const DWORD*, const DWORD*, const DWORD*, BOOL, char**)>("ValidateVertexShader");
    return f ? f(code, r1, r2, errors, out) : E_FAIL;
}

extern "C" void WINAPI DebugSetMute() {
    auto f = real_fn<void(WINAPI*)()>("DebugSetMute");
    if (f) f();
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
