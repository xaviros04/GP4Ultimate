/* Minimal fake d3d8 for testing the GP4 Ultimate d3d8 proxy's vtable hooks without a GPU.
 * Implements just the slots the proxy and its test call; every other slot is NULL. */
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>
#include <string.h>

static D3DMATRIX g_proj;
static LONG g_presents, g_draws;
static D3DPRESENT_PARAMETERS g_last_pp;   /* what reached the "driver" (CreateDevice/Reset) */

static ULONG WINAPI addref(void* self) { (void)self; return 1; }
static ULONG WINAPI release(void* self) { (void)self; return 0; }

/* device */
static HRESULT WINAPI dev_set_transform(IDirect3DDevice8* d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m) {
    (void)d;
    if (t == D3DTS_PROJECTION) g_proj = *m;
    return D3D_OK;
}
static HRESULT WINAPI dev_get_transform(IDirect3DDevice8* d, D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) {
    (void)d; (void)t;
    *m = g_proj;
    return D3D_OK;
}
static HRESULT WINAPI dev_present(IDirect3DDevice8* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    (void)d; (void)a; (void)b; (void)w; (void)r;
    InterlockedIncrement(&g_presents);
    return D3D_OK;
}
static HRESULT WINAPI dev_set_vs(IDirect3DDevice8* d, DWORD h) { (void)d; (void)h; return D3D_OK; }
static HRESULT WINAPI dev_draw(IDirect3DDevice8* d, D3DPRIMITIVETYPE t, UINT s, UINT n) {
    (void)d; (void)t; (void)s; (void)n;
    InterlockedIncrement(&g_draws);
    return D3D_OK;
}
static HRESULT WINAPI dev_scene(IDirect3DDevice8* d) { (void)d; return D3D_OK; }
static HRESULT WINAPI dev_reset(IDirect3DDevice8* d, D3DPRESENT_PARAMETERS* pp) {
    (void)d;
    /* Real D3D8 rule: a windowed device has no refresh rate and the default interval. */
    if (pp->Windowed && (pp->FullScreen_RefreshRateInHz || pp->FullScreen_PresentationInterval)) return D3DERR_INVALIDCALL;
    g_last_pp = *pp;
    return D3D_OK;
}

static IDirect3DDevice8Vtbl g_dev_vt;
static IDirect3DDevice8 g_dev = {&g_dev_vt};

/* d3d */
static HRESULT WINAPI d3d_ident(IDirect3D8* d, UINT a, DWORD f, D3DADAPTER_IDENTIFIER8* id) {
    (void)d; (void)a; (void)f;
    memset(id, 0, sizeof *id);
    lstrcpyA(id->Description, "Mock Adapter");
    return D3D_OK;
}
/* Like a modern driver: 50 sizes x 3 refresh rates x 2 formats (300 modes), then the
   ultrawide and the Surround desktop past GP4's 256-mode limit. */
/* Adapter 1: a second monitor with a short list of its own. */
static UINT WINAPI d3d_mode_count(IDirect3D8* d, UINT a) { (void)d; return a == 1 ? 2 : 300 + 3; }
static HRESULT WINAPI d3d_enum_mode(IDirect3D8* d, UINT a, UINT i, D3DDISPLAYMODE* m) {
    static const UINT rr[3] = {60, 75, 144};
    (void)d;
    if (a == 1) {
        if (i >= 2) return D3DERR_INVALIDCALL;
        m->Width = i ? 2560 : 1024; m->Height = i ? 1080 : 768; m->RefreshRate = 60; m->Format = D3DFMT_X8R8G8B8;
        return D3D_OK;
    }
    if (i >= 303) return D3DERR_INVALIDCALL;
    if (i < 300) {
        UINT size = i / 6, rest = i % 6;
        m->Width = 640 + 16 * size; m->Height = 480 + 8 * size;
        m->RefreshRate = rr[rest / 2]; m->Format = (rest % 2) ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8;
        return D3D_OK;
    }
    m->Format = D3DFMT_X8R8G8B8;
    if (i == 300) { m->Width = 2560; m->Height = 1080; m->RefreshRate = 144; }
    else if (i == 301) { m->Width = 2560; m->Height = 1080; m->RefreshRate = 60; }
    else { m->Width = 5760; m->Height = 1080; m->RefreshRate = 60; }
    return D3D_OK;
}
static HRESULT WINAPI d3d_mode(IDirect3D8* d, UINT a, D3DDISPLAYMODE* m) {
    (void)d; (void)a;
    m->Width = 5760; m->Height = 1080; m->RefreshRate = 60; m->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}
static HRESULT WINAPI d3d_create(IDirect3D8* d, UINT a, D3DDEVTYPE t, HWND w, DWORD f, D3DPRESENT_PARAMETERS* pp,
                                 IDirect3DDevice8** out) {
    (void)d; (void)a; (void)t; (void)w; (void)f;
    if (pp->Windowed && (pp->FullScreen_RefreshRateInHz || pp->FullScreen_PresentationInterval)) return D3DERR_INVALIDCALL;
    g_last_pp = *pp;
    *out = &g_dev;
    return D3D_OK;
}
static IDirect3D8Vtbl g_d3d_vt;
static IDirect3D8 g_d3d = {&g_d3d_vt};

IDirect3D8* WINAPI Direct3DCreate8(UINT sdk) {
    (void)sdk;
    g_d3d_vt.AddRef = (void*)addref;
    g_d3d_vt.Release = (void*)release;
    g_d3d_vt.GetAdapterIdentifier = d3d_ident;
    g_d3d_vt.CreateDevice = d3d_create;
    g_d3d_vt.GetAdapterDisplayMode = d3d_mode;
    g_d3d_vt.GetAdapterModeCount = d3d_mode_count;
    g_d3d_vt.EnumAdapterModes = d3d_enum_mode;
    g_dev_vt.Reset = dev_reset;
    g_dev_vt.AddRef = (void*)addref;
    g_dev_vt.Release = (void*)release;
    g_dev_vt.SetTransform = dev_set_transform;
    g_dev_vt.GetTransform = dev_get_transform;
    g_dev_vt.Present = dev_present;
    g_dev_vt.SetVertexShader = dev_set_vs;
    g_dev_vt.DrawPrimitive = dev_draw;
    g_dev_vt.BeginScene = dev_scene;
    g_dev_vt.EndScene = dev_scene;
    return &g_d3d;
}

LONG mock_presents(void) { return g_presents; }
LONG mock_draws(void) { return g_draws; }
void mock_last_pp(D3DPRESENT_PARAMETERS* out) { *out = g_last_pp; }
