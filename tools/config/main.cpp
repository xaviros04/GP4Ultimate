// gp4ultimate-config.exe - graphical configurator for the GP4 mod (gp4ultimate.ini).
// Sits in the game folder next to dinput8.dll/d3d8.dll. Win32 + Direct3D 9 + Dear ImGui,
// one self-contained exe.
//
//   gp4ultimate-config [--ini path] [--page name] [--advanced]
//   test/doc options: --dinput dll  --screenshot out.bmp [--frames n] [--size WxH]
//                     --selftest (with --dinput mock_dinput.dll --ini <new file>)
#include <d3d9.h>

#include <cstdio>
#include <string>

#include "app.h"
#include "core/version.h"
#include "core/paths.h"
#include "i18n.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "resource.h"
#include "selftest.h"

using gp4cfg::T;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

LPDIRECT3D9 g_d3d = nullptr;
LPDIRECT3DDEVICE9 g_dev = nullptr;
D3DPRESENT_PARAMETERS g_pp{};
UINT g_resize_w = 0, g_resize_h = 0;
gp4cfg::App g_app;
bool g_quit = false;

std::wstring wide(const char* utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

bool create_device(HWND hwnd) {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return false;
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.EnableAutoDepthStencil = TRUE;
    g_pp.AutoDepthStencilFormat = D3DFMT_D16;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_dev) < 0)
        return false;
    return true;
}

void reset_device() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    if (g_dev->Reset(&g_pp) == D3DERR_INVALIDCALL) IM_ASSERT(0);
    ImGui_ImplDX9_CreateDeviceObjects();
}

std::string load_resource(int id) {
    HRSRC r = FindResourceA(nullptr, MAKEINTRESOURCEA(id), RT_RCDATA);
    if (!r) return "";
    HGLOBAL g = LoadResource(nullptr, r);
    const char* p = g ? (const char*)LockResource(g) : nullptr;
    return p ? std::string(p, SizeofResource(nullptr, r)) : "";
}

// Saves the back buffer as a 24-bit BMP (test and documentation screenshots).
bool save_screenshot(const std::string& path) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(g_dev->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            FILE* f = std::fopen(path.c_str(), "wb");
            if (f) {
                int row = ((int)d.Width * 3 + 3) & ~3;
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof fh + sizeof ih;
                fh.bfSize = fh.bfOffBits + row * d.Height;
                ih.biSize = sizeof ih;
                ih.biWidth = (LONG)d.Width;
                ih.biHeight = (LONG)d.Height;
                ih.biPlanes = 1;
                ih.biBitCount = 24;
                std::fwrite(&fh, sizeof fh, 1, f);
                std::fwrite(&ih, sizeof ih, 1, f);
                std::string line((size_t)row, '\0');
                for (int y = (int)d.Height - 1; y >= 0; --y) {
                    const unsigned char* src = (const unsigned char*)lr.pBits + y * lr.Pitch;
                    for (UINT x = 0; x < d.Width; ++x) {
                        line[x * 3 + 0] = (char)src[x * 4 + 0];
                        line[x * 3 + 1] = (char)src[x * 4 + 1];
                        line[x * 3 + 2] = (char)src[x * 4 + 2];
                    }
                    std::fwrite(line.data(), 1, line.size(), f);
                }
                std::fclose(f);
                ok = true;
            }
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    bb->Release();
    return ok;
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resize_w = (UINT)LOWORD(lp);
            g_resize_h = (UINT)HIWORD(lp);
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            if (g_app.dirty()) {
                int r = MessageBoxW(hwnd, wide(T("There are unsaved changes in gp4ultimate.ini.\n\nSave them before quitting?")).c_str(),
                                    wide((std::string(T("GP4 Ultimate configurator")) + " " + gp4u::kVersion).c_str()).c_str(), MB_YESNOCANCEL | MB_ICONQUESTION);
                if (r == IDCANCEL) return 0;
                if (r == IDYES && !g_app.save()) return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

float dpi_scale(HWND hwnd) {
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    auto f = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    UINT dpi = f ? f(hwnd) : 96;
    if (!dpi) {
        HDC dc = GetDC(hwnd);
        dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(hwnd, dc);
    }
    return dpi ? dpi / 96.0f : 1.0f;
}

void setup_style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.PopupRounding = 6;
    s.TabRounding = 6;
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 7);
    s.CellPadding = ImVec2(6, 4);
    s.WindowPadding = ImVec2(12, 10);
    s.SeparatorTextBorderSize = 2;
    ImVec4* c = s.Colors;
    ImVec4 accent(0.86f, 0.20f, 0.18f, 1.0f);   // racing red
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.14f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.19f, 0.23f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.25f, 0.30f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.29f, 0.35f, 1);
    c[ImGuiCol_Button] = ImVec4(0.22f, 0.23f, 0.28f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.70f, 0.18f, 0.16f, 1);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.21f, 0.26f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.20f, 0.21f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.45f, 0.18f, 0.18f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.16f, 0.17f, 0.21f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.70f, 0.18f, 0.16f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.62f, 0.15f, 0.14f, 1);
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.35f, 0.30f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.40f, 0.35f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.32f, 0.22f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.31f, 0.36f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    s.ScaleAllSizes(scale);
}

void load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    const char* names[] = {"segoeui.ttf", "tahoma.ttf", "arial.ttf", "verdana.ttf"};
    for (const char* n : names) {
        std::string p = std::string(win) + "\\Fonts\\" + n;
        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES && io.Fonts->AddFontFromFileTTF(p.c_str(), 16.0f * scale))
            return;
    }
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
}

std::string arg_value(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == name) return argv[i + 1];
    return "";
}

bool has_flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == name) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    gp4cfg::Options opt;
    opt.ini_path = arg_value(argc, argv, "--ini");
    opt.dinput_dll = arg_value(argc, argv, "--dinput");
    opt.start_page = arg_value(argc, argv, "--page");
    opt.screenshot = arg_value(argc, argv, "--screenshot");
    opt.advanced = has_flag(argc, argv, "--advanced");
    std::string frames = arg_value(argc, argv, "--frames");
    if (!frames.empty()) opt.frames = std::max(1, atoi(frames.c_str()));
    std::string size = arg_value(argc, argv, "--size");
    if (!size.empty()) std::sscanf(size.c_str(), "%dx%d", &opt.width, &opt.height);

    // A first guess at the language, for the window title and an early error; App::init sets
    // it for good from the document it loads.
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir = exe;
        dir = dir.substr(0, dir.find_last_of("\\/"));
        std::string ini = opt.ini_path.empty() ? gp4u::config_file(dir) : opt.ini_path;
        gp4cfg::set_current_language(gp4cfg::resolve_language(gp4cfg::ini_language(ini)));
    }

    typedef BOOL(WINAPI * SetDpiAwareFn)();
    if (auto f = (SetDpiAwareFn)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware")) f();

    WNDCLASSEXW wc = {sizeof wc, CS_CLASSDC, wnd_proc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
                      L"gp4ultimate-config", nullptr};
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, wide((std::string(T("GP4 Ultimate configurator")) + " " + gp4u::kVersion).c_str()).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              opt.width, opt.height, nullptr, nullptr, wc.hInstance, nullptr);
    float scale = opt.screenshot.empty() ? dpi_scale(hwnd) : 1.0f;
    if (scale > 1.01f && opt.screenshot.empty())
        SetWindowPos(hwnd, nullptr, 0, 0, (int)(opt.width * scale), (int)(opt.height * scale), SWP_NOMOVE | SWP_NOZORDER);
    if (!create_device(hwnd)) {
        MessageBoxW(hwnd, wide(T("Could not start Direct3D 9.")).c_str(), wide((std::string(T("GP4 Ultimate configurator")) + " " + gp4u::kVersion).c_str()).c_str(), MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // no imgui.ini in the game folder
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    setup_style(scale);
    load_fonts(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_dev);

    g_app.init(hwnd, opt, load_resource(IDR_SCHEMA));
    gp4cfg::SelfTest* selftest = nullptr;
    if (has_flag(argc, argv, "--selftest")) selftest = new gp4cfg::SelfTest(opt.ini_path, opt.ini_path + ".selftest.txt");

    LARGE_INTEGER freq, t0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    int frame = 0;
    int code = 0;
    std::string title_lang = g_app.language();
    SetWindowTextW(hwnd, wide((std::string(T("GP4 Ultimate configurator")) + " " + gp4u::kVersion).c_str()).c_str());
    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_quit = true;
        }
        if (g_quit) break;
        if (g_resize_w && g_resize_h) {
            g_pp.BackBufferWidth = g_resize_w;
            g_pp.BackBufferHeight = g_resize_h;
            g_resize_w = g_resize_h = 0;
            reset_device();
        }
        HRESULT coop = g_dev->TestCooperativeLevel();
        if (coop == D3DERR_DEVICELOST) { Sleep(10); continue; }
        if (coop == D3DERR_DEVICENOTRESET) reset_device();

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
        if (selftest && selftest->tick(g_app, t)) {
            code = selftest->failures() ? 1 : 0;
            ImGui::EndFrame();
            break;
        }
        g_app.frame(t);
        if (g_app.language() != title_lang) {   // the language selector was used
            title_lang = g_app.language();
            SetWindowTextW(hwnd, wide((std::string(T("GP4 Ultimate configurator")) + " " + gp4u::kVersion).c_str()).c_str());
        }
        ImGui::EndFrame();
        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_RGBA(23, 25, 30, 255), 1.0f, 0);
        if (g_dev->BeginScene() >= 0) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_dev->EndScene();
        }
        if (!opt.screenshot.empty() && ++frame >= opt.frames) {
            code = save_screenshot(opt.screenshot) ? 0 : 2;
            break;
        }
        HRESULT r = g_dev->Present(nullptr, nullptr, nullptr, nullptr);
        if (r == D3DERR_DEVICELOST) Sleep(10);
    }

    g_app.shutdown();
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_dev) g_dev->Release();
    if (g_d3d) g_d3d->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return code;
}
