// Test of the GP4 Ultimate d3d8 proxy: d3d8.dll (ours) chained to mock_d3d8.dll. Checks that the
// device hooks are installed, the aspect diagnosis is logged, Hor+ correction reaches the
// driver when enabled and is left alone when not, and draws/presents still go through.
//   test_d3d8 fix | test_d3d8 nofix | test_d3d8 window
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// [display] window = 1: GP4's fullscreen request becomes a centred borderless window that
// stays put, on CreateDevice and on Reset.
static int run_window() {
    {
        std::ofstream f("gp4ultimate.ini");
        f << "[display]\nchain = mock_d3d8.dll\nwindow = 1\nresolution = 1920x1080\n";
    }
    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, "STATIC", "GP4", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, nullptr,
                                nullptr, GetModuleHandleA(nullptr), nullptr);
    CHECK(hwnd != nullptr);
    if (!hwnd) return 1;
    IDirect3D8* d3d = Direct3DCreate8(D3D_SDK_VERSION);
    CHECK(d3d != nullptr);
    if (!d3d) return 1;
    // GP4 builds its mode list itself (first 256 modes): in window mode it gets a short
    // list starting with the configured resolution, in 32 and 16 bit.
    UINT nmodes = d3d->GetAdapterModeCount(0);
    CHECK(nmodes >= 20 && nmodes < 256 && nmodes % 2 == 0);
    D3DDISPLAYMODE dm{};
    CHECK(SUCCEEDED(d3d->EnumAdapterModes(0, 0, &dm)) && dm.Width == 1920 && dm.Height == 1080 && dm.Format == D3DFMT_X8R8G8B8);
    CHECK(SUCCEEDED(d3d->EnumAdapterModes(0, 1, &dm)) && dm.Width == 1920 && dm.Format == D3DFMT_R5G6B5);
    bool has_desktop = false;
    for (UINT i = 0; i < nmodes; ++i)
        if (SUCCEEDED(d3d->EnumAdapterModes(0, i, &dm)) && dm.Width == 5760 && dm.Height == 1080) has_desktop = true;
    CHECK(has_desktop);
    CHECK(d3d->EnumAdapterModes(0, nmodes, &dm) == D3DERR_INVALIDCALL);
    auto last_pp = (void (*)(D3DPRESENT_PARAMETERS*))GetProcAddress(GetModuleHandleA("mock_d3d8.dll"), "mock_last_pp");
    CHECK(last_pp != nullptr);
    if (!last_pp) return 1;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    auto centred = [&](int w, int h) {
        RECT r;
        GetWindowRect(hwnd, &r);
        int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        return r.left == mi.rcMonitor.left + (mw - w) / 2 && r.top == mi.rcMonitor.top + (mh - h) / 2 &&
               r.right - r.left == w && r.bottom - r.top == h;
    };

    // What GP4 asks for: exclusive fullscreen, 16-bit, 60 Hz, vsync.
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = FALSE;
    pp.BackBufferWidth = 1024;
    pp.BackBufferHeight = 768;
    pp.BackBufferFormat = D3DFMT_R5G6B5;
    pp.FullScreen_RefreshRateInHz = 60;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    pp.SwapEffect = D3DSWAPEFFECT_FLIP;
    pp.hDeviceWindow = hwnd;
    IDirect3DDevice8* dev = nullptr;
    CHECK(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, 0, &pp, &dev)) && dev);
    if (!dev) return 1;
    D3DPRESENT_PARAMETERS got{};
    last_pp(&got);
    CHECK(got.Windowed == TRUE);
    CHECK(got.BackBufferFormat == D3DFMT_X8R8G8B8);   // the desktop's
    CHECK(got.FullScreen_RefreshRateInHz == 0 && got.FullScreen_PresentationInterval == 0);
    CHECK(got.BackBufferWidth == 1024 && got.BackBufferHeight == 768);
    CHECK(centred(1024, 768));
    CHECK(!(GetWindowLongA(hwnd, GWL_STYLE) & WS_CAPTION));
    CHECK(!(GetWindowLongA(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST));
    // The game tries to move it to the corner: it stays.
    SetWindowPos(hwnd, nullptr, 0, 0, 800, 600, SWP_NOZORDER);
    CHECK(centred(1024, 768));
    // Resolution change through Reset.
    pp.Windowed = FALSE;
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 960;
    pp.FullScreen_RefreshRateInHz = 75;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    CHECK(SUCCEEDED(dev->Reset(&pp)));
    last_pp(&got);
    CHECK(got.Windowed == TRUE && got.BackBufferWidth == 1280);
    CHECK(centred(1280, 960));

    std::ifstream lf("gp4ultimate_d3d.log");
    std::stringstream ls;
    ls << lf.rdbuf();
    CHECK(ls.str().find("using a window") != std::string::npos);
    CHECK(ls.str().find("resolution: GP4.exe not recognised") != std::string::npos);   // this test is not GP4
    DestroyWindow(hwnd);
    std::printf("[window] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// Full screen with an ultrawide resolution the driver lists past GP4's first 256 modes:
// GP4 gets the real modes once per size and format, the configured one first.
static int run_fullscreen() {
    {
        std::ofstream f("gp4ultimate.ini");
        f << "[display]\nchain = mock_d3d8.dll\nresolution = 2560x1080\n";
    }
    IDirect3D8* d3d = Direct3DCreate8(D3D_SDK_VERSION);
    CHECK(d3d != nullptr);
    if (!d3d) return 1;
    UINT n = d3d->GetAdapterModeCount(0);
    CHECK(n == 50 * 2 + 2);   // 50 sizes x 2 formats + 2560x1080 + 5760x1080
    D3DDISPLAYMODE dm{};
    CHECK(SUCCEEDED(d3d->EnumAdapterModes(0, 0, &dm)) && dm.Width == 2560 && dm.Height == 1080 && dm.RefreshRate == 144 &&
          dm.Format == D3DFMT_X8R8G8B8);   // the first-listed refresh, as GP4 itself would keep
    bool desk = false;
    for (UINT i = 0; i < n; ++i)
        if (SUCCEEDED(d3d->EnumAdapterModes(0, i, &dm)) && dm.Width == 5760) desk = true;
    CHECK(desk);
    CHECK(d3d->EnumAdapterModes(0, n, &dm) == D3DERR_INVALIDCALL);
    // A second adapter has its own list; there 2560x1080 is within reach: untouched.
    CHECK(d3d->GetAdapterModeCount(1) == 2);
    CHECK(SUCCEEDED(d3d->EnumAdapterModes(1, 1, &dm)) && dm.Width == 2560);
    std::ifstream lf("gp4ultimate_d3d.log");
    std::stringstream ls;
    ls << lf.rdbuf();
    CHECK(ls.str().find("adapter 0 lists 303 modes and 2560x1080 is past the 256 GP4 reads; GP4 gets 102") !=
          std::string::npos);
    CHECK(ls.str().find("adapter 1 lists 2560x1080 within the first 256 modes") != std::string::npos);
    std::printf("[fullscreen] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// Full screen without [display] resolution: the driver's list goes through untouched.
static int run_fullscreen_plain() {
    {
        std::ofstream f("gp4ultimate.ini");
        f << "[display]\nchain = mock_d3d8.dll\n";
    }
    IDirect3D8* d3d = Direct3DCreate8(D3D_SDK_VERSION);
    CHECK(d3d != nullptr);
    if (!d3d) return 1;
    CHECK(d3d->GetAdapterModeCount(0) == 303);
    D3DDISPLAYMODE dm{};
    CHECK(SUCCEEDED(d3d->EnumAdapterModes(0, 0, &dm)) && dm.Width == 640 && dm.RefreshRate == 60);
    std::printf("[fullscreen_plain] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "window") return run_window();
    if (argc > 1 && std::string(argv[1]) == "fullscreen_plain") return run_fullscreen_plain();
    if (argc > 1 && std::string(argv[1]) == "fullscreen") return run_fullscreen();
    bool fix = argc > 1 && std::string(argv[1]) == "fix";
    {
        std::ofstream f("gp4ultimate.ini");
        f << "[display]\nchain = mock_d3d8.dll\naspect_fix = " << (fix ? 1 : 0) << "\n";
    }
    IDirect3D8* d3d = Direct3DCreate8(D3D_SDK_VERSION);
    CHECK(d3d != nullptr);
    if (!d3d) return 1;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.BackBufferWidth = 5760;
    pp.BackBufferHeight = 1080;
    pp.hDeviceWindow = (HWND)0x1234;
    IDirect3DDevice8* dev = nullptr;
    CHECK(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, (HWND)0x1234, 0, &pp, &dev)) && dev);
    if (!dev) return 1;

    // GP4-like 4:3 projection.
    D3DMATRIX m{};
    float y = 1.0f / std::tan(0.39f);
    m._11 = y / (4.0f / 3.0f);
    m._22 = y;
    m._33 = 1.001f;
    m._34 = 1.0f;
    m._43 = -1.0f;
    dev->SetTransform(D3DTS_PROJECTION, &m);
    D3DMATRIX got{};
    dev->GetTransform(D3DTS_PROJECTION, &got);
    double aspect = got._22 / got._11;
    if (fix) CHECK(std::fabs(aspect - 5760.0 / 1080.0) < 1e-3);
    else CHECK(std::fabs(aspect - 4.0 / 3.0) < 1e-3);
    CHECK(std::fabs(got._22 - y) < 1e-6);   // vertical FOV untouched

    HMODULE mock = GetModuleHandleA("mock_d3d8.dll");
    CHECK(mock != nullptr);
    auto presents = (LONG(*)())GetProcAddress(mock, "mock_presents");
    auto draws = (LONG(*)())GetProcAddress(mock, "mock_draws");
    dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
    dev->Present(nullptr, nullptr, nullptr, nullptr);
    CHECK(presents && presents() == 1);
    CHECK(draws && draws() == 1);

    std::ifstream lf("gp4ultimate_d3d.log");
    std::stringstream ls;
    ls << lf.rdbuf();
    std::string log = ls.str();
    CHECK(log.find("device hooks installed") != std::string::npos);
    CHECK(log.find("back buffer 5760x1080") != std::string::npos);
    CHECK(log.find(fix ? "correcting (Hor+)" : "STRETCHED") != std::string::npos);
    std::printf("[%s] %d passed, %d failed\n", fix ? "fix" : "nofix", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
