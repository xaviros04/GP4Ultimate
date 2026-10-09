// gp4ultimate-devices - find out what your wheel, pedals and shifter look like to DirectInput,
// and write the [devices]/[axes]/[buttons] part of gp4ultimate.ini.
//
//   gp4ultimate-devices              list attached game controllers
//   gp4ultimate-devices watch        live axes/buttons of every controller (Esc to quit);
//                                    buttons are numbered from 0, as in gp4ultimate.ini (b0, b1...)
//   gp4ultimate-devices ini          print a suggested gp4ultimate.ini mapping
//   gp4ultimate-devices ffb <n>      push wheel <n> right, then left (checks the FF sign)
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <cstdarg>
#include <conio.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config/i18n.h"   // same language rule and table as gp4ultimate-config
#include "core/paths.h"

using gp4cfg::T;

namespace {

struct Dev {
    DIDEVICEINSTANCEA inst;
    IDirectInputDevice8A* dev = nullptr;
    DIDEVCAPS caps{};
    bool axis[8] = {};
    DIJOYSTATE2 st{};
};

IDirectInput8A* g_di = nullptr;
HWND g_hwnd = nullptr;
FILE* g_copy = nullptr;   // when set, out() also writes here

void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    if (g_copy) {
        va_start(ap, fmt);
        std::vfprintf(g_copy, fmt, ap);
        va_end(ap);
    }
}
std::vector<Dev> g_devs;
const char* kAxis[8] = {"X", "Y", "Z", "RX", "RY", "RZ", "S0", "S1"};

BOOL CALLBACK enum_dev(LPCDIDEVICEINSTANCEA i, LPVOID) {
    Dev d;
    d.inst = *i;
    g_devs.push_back(d);
    return DIENUM_CONTINUE;
}

BOOL CALLBACK enum_obj(LPCDIDEVICEOBJECTINSTANCEA o, LPVOID r) {
    Dev* d = (Dev*)r;
    const GUID* g[6] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis};
    for (int a = 0; a < 6; ++a)
        if (o->guidType == *g[a]) d->axis[a] = true;
    if (o->guidType == GUID_Slider) (d->axis[6] ? d->axis[7] : d->axis[6]) = true;
    return DIENUM_CONTINUE;
}

std::string guid_str(const GUID& g) {
    char b[64];
    std::snprintf(b, sizeof b, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, g.Data2,
                  g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

unsigned vid(const Dev& d) { return d.inst.guidProduct.Data1 & 0xFFFF; }
unsigned pid(const Dev& d) { return d.inst.guidProduct.Data1 >> 16; }

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool open_all(HWND hwnd, bool exclusive_ffb) {
    // Load Windows' own dinput8.dll explicitly: this tool usually sits in the game folder,
    // next to the mod's dinput8.dll proxy, and must list the REAL devices.
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    lstrcatA(sys, "\\dinput8.dll");
    HMODULE m = LoadLibraryA(sys);
    typedef HRESULT(WINAPI * Create8)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    Create8 create = m ? (Create8)GetProcAddress(m, "DirectInput8Create") : nullptr;
    if (!create || FAILED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&g_di, nullptr))) {
        std::printf("DirectInput8Create failed\n");
        return false;
    }
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_dev, nullptr, DIEDFL_ATTACHEDONLY);
    for (auto& d : g_devs) {
        if (FAILED(g_di->CreateDevice(d.inst.guidInstance, &d.dev, nullptr))) { d.dev = nullptr; continue; }
        d.dev->SetDataFormat(&c_dfDIJoystick2);
        DIPROPRANGE r{};
        r.diph.dwSize = sizeof r; r.diph.dwHeaderSize = sizeof r.diph; r.diph.dwHow = DIPH_DEVICE;
        r.lMin = 0; r.lMax = 65535;
        d.dev->SetProperty(DIPROP_RANGE, &r.diph);
        d.caps.dwSize = sizeof d.caps;
        d.dev->GetCapabilities(&d.caps);
        d.dev->EnumObjects(enum_obj, &d, DIDFT_AXIS);
        bool ff = (d.caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
        d.dev->SetCooperativeLevel(hwnd, (ff && exclusive_ffb ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_BACKGROUND);
        d.dev->Acquire();
    }
    return true;
}

void poll(Dev& d) {
    if (!d.dev) return;
    d.dev->Poll();
    if (FAILED(d.dev->GetDeviceState(sizeof d.st, &d.st))) {
        d.dev->Acquire();
        d.dev->GetDeviceState(sizeof d.st, &d.st);
    }
}

LONG axis_val(const DIJOYSTATE2& s, int a) {
    const LONG v[8] = {s.lX, s.lY, s.lZ, s.lRx, s.lRy, s.lRz, s.rglSlider[0], s.rglSlider[1]};
    return v[a];
}

void list() {
    out(T("%u game controller(s)\n\n"), (unsigned)g_devs.size());
    for (size_t i = 0; i < g_devs.size(); ++i) {
        Dev& d = g_devs[i];
        out("[%u] %s\n    vidpid:%04X:%04X   guid:%s\n", (unsigned)i, d.inst.tszProductName, vid(d), pid(d),
                    guid_str(d.inst.guidInstance).c_str());
        out("%s", T("    axes:"));
        for (int a = 0; a < 8; ++a)
            if (d.axis[a]) out(" %s", kAxis[a]);
        out(T("   buttons: %lu   povs: %lu%s\n\n"), (unsigned long)d.caps.dwButtons, (unsigned long)d.caps.dwPOVs,
                    (d.caps.dwFlags & DIDC_FORCEFEEDBACK) ? "   FORCE FEEDBACK" : "");
    }
}

void watch() {
    std::printf("%s", T("Move each control and note which device/axis/button changes. Esc to stop.\n\n"));
    for (;;) {
        if (_kbhit() && _getch() == 27) { std::printf("\x1b[%uB\n", (unsigned)g_devs.size()); return; }
        std::string out;
        for (size_t i = 0; i < g_devs.size(); ++i) {
            Dev& d = g_devs[i];
            poll(d);
            char line[512];
            int n = std::snprintf(line, sizeof line, "[%u] %-22.22s", (unsigned)i, d.inst.tszProductName);
            for (int a = 0; a < 8; ++a)
                if (d.axis[a]) n += std::snprintf(line + n, sizeof line - n, " %s:%5.1f%%", kAxis[a], axis_val(d.st, a) / 655.35);
            n += std::snprintf(line + n, sizeof line - n, "  btn:");
            int shown = 0;
            for (int b = 0; b < 128 && n < 480; ++b)
                if (d.st.rgbButtons[b] & 0x80) { n += std::snprintf(line + n, sizeof line - n, " %d", b); ++shown; }
            if (!shown) n += std::snprintf(line + n, sizeof line - n, " -");
            for (DWORD p = 0; p < d.caps.dwPOVs && p < 4; ++p)
                if (LOWORD(d.st.rgdwPOV[p]) != 0xFFFF) n += std::snprintf(line + n, sizeof line - n, " pov%lu:%lu", (unsigned long)p, (unsigned long)d.st.rgdwPOV[p] / 100);
            out += line;
            out += "                    \n";
        }
        std::printf("%s\x1b[%uA", out.c_str(), (unsigned)g_devs.size());
        std::fflush(stdout);
        Sleep(50);
    }
}

void suggest_ini() {
    // Heuristics: the FF device is the wheel; a device named like pedals is the pedal set;
    // everything else contributes buttons (shifters, button boxes).
    int wheel = -1, pedals = -1;
    for (size_t i = 0; i < g_devs.size(); ++i)
        if (wheel < 0 && (g_devs[i].caps.dwFlags & DIDC_FORCEFEEDBACK)) wheel = (int)i;
    for (size_t i = 0; i < g_devs.size(); ++i) {
        std::string n = lower(g_devs[i].inst.tszProductName);
        if ((int)i != wheel && pedals < 0 && n.find("pedal") != std::string::npos)
            pedals = (int)i;
    }
    if (wheel < 0 && !g_devs.empty()) wheel = 0;
    out("; Suggested by gp4ultimate-devices - check every axis with `gp4ultimate-devices watch`.\n[devices]\n");
    std::vector<std::string> aliases(g_devs.size());
    int extra = 0;
    for (size_t i = 0; i < g_devs.size(); ++i) {
        aliases[i] = (int)i == wheel ? "wheel" : (int)i == pedals ? "pedals" : "aux" + std::to_string(++extra);
        bool dup = false;
        for (size_t j = 0; j < g_devs.size(); ++j)
            if (j != i && g_devs[j].inst.guidProduct == g_devs[i].inst.guidProduct) dup = true;
        if (dup) out("%s = guid:%s   ; %s\n", aliases[i].c_str(), guid_str(g_devs[i].inst.guidInstance).c_str(), g_devs[i].inst.tszProductName);
        else out("%s = vidpid:%04X:%04X   ; %s\n", aliases[i].c_str(), vid(g_devs[i]), pid(g_devs[i]), g_devs[i].inst.tszProductName);
    }
    out("\n[virtual]\nname = GP4 Composite Wheel\n");
    if (wheel >= 0 && (g_devs[wheel].caps.dwFlags & DIDC_FORCEFEEDBACK)) out("ffb = wheel\n");
    out("\n[axes]\n");
    if (wheel >= 0) out("X = wheel.X\n");
    int src = pedals >= 0 ? pedals : wheel;
    if (src >= 0) {
        const char* al = aliases[src].c_str();
        // Typical layouts: throttle/brake/clutch on Y/RZ/Z (wheel bases) or X/Y/Z (pedal sets).
        std::vector<int> av;
        for (int a = 0; a < 8; ++a)
            if (g_devs[src].axis[a] && !(src == wheel && a == 0)) av.push_back(a);
        const char* role[3] = {"accelerator", "brake", "clutch"};
        const char* target[3] = {"Y", "RZ", "Z"};
        for (size_t k = 0; k < av.size() && k < 3; ++k)
            out("%s = %s.%s invert   ; %s? (check direction with watch)\n", target[k], al, kAxis[av[k]], role[k]);
    }
    out("\n[buttons]\nauto =");
    bool first = true;
    for (size_t i = 0; i < g_devs.size(); ++i)
        if (g_devs[i].caps.dwButtons) { out("%s %s", first ? "" : ",", aliases[i].c_str()); first = false; }
    out("\n");
    for (size_t i = 0; i < g_devs.size(); ++i)
        if (g_devs[i].caps.dwPOVs) { out("\n[povs]\n0 = %s.pov0\n", aliases[i].c_str()); break; }
}

void test_ffb(int n) {
    if (n < 0 || n >= (int)g_devs.size() || !g_devs[n].dev || !(g_devs[n].caps.dwFlags & DIDC_FORCEFEEDBACK)) {
        std::printf(T("device %d has no force feedback\n"), n);
        return;
    }
    IDirectInputDevice8A* dev = g_devs[n].dev;
    DIPROPDWORD p{};
    p.diph.dwSize = sizeof p; p.diph.dwHeaderSize = sizeof p.diph; p.diph.dwHow = DIPH_DEVICE;
    p.dwData = DIPROPAUTOCENTER_OFF;
    dev->Unacquire();
    dev->SetCooperativeLevel(g_hwnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);   // FF needs exclusive access
    dev->SetProperty(DIPROP_AUTOCENTER, &p.diph);
    dev->Acquire();
    DWORD axes[1] = {DIJOFS_X};
    LONG dir[1] = {0};
    DICONSTANTFORCE cf{0};
    DIEFFECT e{};
    e.dwSize = sizeof e; e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS; e.dwDuration = INFINITE;
    e.dwGain = DI_FFNOMINALMAX; e.dwTriggerButton = DIEB_NOTRIGGER; e.cAxes = 1; e.rgdwAxes = axes;
    e.rglDirection = dir; e.cbTypeSpecificParams = sizeof cf; e.lpvTypeSpecificParams = &cf;
    IDirectInputEffect* fx = nullptr;
    HRESULT hr = dev->CreateEffect(GUID_ConstantForce, &e, &fx, nullptr);
    if (FAILED(hr)) { std::printf("CreateEffect failed 0x%08lx\n", (unsigned long)hr); return; }
    fx->Start(1, 0);
    std::printf("%s", T("Let go of the wheel. Positive force (+30%) for 1.5 s...\n"));
    poll(g_devs[n]);
    LONG x0 = g_devs[n].st.lX;
    cf.lMagnitude = 3000;
    fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS);
    Sleep(1500);
    poll(g_devs[n]);
    LONG x1 = g_devs[n].st.lX;
    std::printf("%s", T("Negative force (-30%) for 1.5 s...\n"));
    cf.lMagnitude = -3000;
    fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS);
    Sleep(1500);
    poll(g_devs[n]);
    LONG x2 = g_devs[n].st.lX;
    cf.lMagnitude = 0;
    fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS);
    fx->Stop();
    fx->Release();
    dev->Unacquire();
    dev->SetCooperativeLevel(g_hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    dev->Acquire();
    std::printf(T("X axis: start %.1f%%, after +force %.1f%%, after -force %.1f%%\n"), x0 / 655.35, x1 / 655.35, x2 / 655.35);
    // DirectInput (and GP4): a positive magnitude pushes towards -X.
    if (x2 > x1 + 3000) std::printf("%s", T("Positive force moves X down (DirectInput standard, as GP4 expects): keep [ffb] invert = 0.\n"));
    else if (x1 > x2 + 3000) std::printf("%s", T("Positive force moves X up (opposite to DirectInput): set [ffb] invert = 1.\n"));
    else std::printf("%s", T("The wheel barely moved (held, or too little force): watch it and set invert by eye -\n"
                             "positive force should turn it the way that DECREASES X in `gp4ultimate-devices watch`.\n"));
}

}  // namespace

// Started by double-click (we are the only process on this console): interactive menu,
// so the window does not just flash and close.
bool own_console() {
    DWORD pids[2];
    return GetConsoleProcessList(pids, 2) == 1;
}

std::string exe_dir() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string d = p;
    return d.substr(0, d.find_last_of("\\/") + 1);
}

void save_and_run(void (*fn)(), const std::string& file) {
    std::string path = exe_dir() + file;
    g_copy = std::fopen(path.c_str(), "w");
    fn();
    if (g_copy) {
        std::fclose(g_copy);
        g_copy = nullptr;
        std::printf(T("\n(Saved to %s)\n"), path.c_str());
    }
}

void menu() {
    for (;;) {
        std::printf("%s", T("\n=== gp4ultimate-devices ===\n"
                            "  1  List devices (saved to gp4ultimate-devices.txt)\n"
                            "  2  Live axes and buttons (Esc to go back)\n"
                            "  3  Suggest a configuration (saved to gp4ultimate_suggested.ini)\n"
                            "  4  Test the force feedback direction\n"
                            "  0  Quit\n"
                            "Option: "));
        std::fflush(stdout);
        int c = _getch();
        std::printf("%c\n\n", c);
        if (c == '0' || c == 27) return;
        if (c == '1') save_and_run(list, "gp4ultimate-devices.txt");
        else if (c == '2') watch();
        else if (c == '3') save_and_run(suggest_ini, "gp4ultimate_suggested.ini");
        else if (c == '4') {
            list();
            std::printf("%s", T("Number of the force feedback wheel: "));
            std::fflush(stdout);
            char buf[16] = {};
            if (std::fgets(buf, sizeof buf, stdin)) test_ffb(std::atoi(buf));
        }
    }
}

int main(int argc, char** argv) {
    // Language: [general] language of the gp4ultimate.ini (or old gp4mod.ini) next to the exe.
    std::string dir = exe_dir();
    gp4cfg::set_current_language(gp4cfg::resolve_language(gp4cfg::ini_language(gp4u::config_file(dir.substr(0, dir.size() - 1)))));
    bool interactive = argc < 2 && own_console();
    std::string cmd = argc > 1 ? argv[1] : "list";
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) hwnd = GetDesktopWindow();
    g_hwnd = hwnd;
    // Enable ANSI cursor movement for `watch` on Windows 10+ consoles.
    DWORD mode = 0;
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(hout, &mode)) SetConsoleMode(hout, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
    if (!open_all(hwnd, cmd == "ffb")) {
        if (interactive) { std::printf("%s", T("\nPress a key to quit...")); _getch(); }
        return 1;
    }
    int rc = 0;
    if (interactive) {
        if (g_devs.empty()) std::printf("%s", T("No connected controller found.\n"));
        menu();
    } else if (cmd == "list") list();
    else if (cmd == "watch") watch();
    else if (cmd == "ini") suggest_ini();
    else if (cmd == "ffb" && argc > 2) test_ffb(std::atoi(argv[2]));
    else { std::printf("usage: gp4ultimate-devices [list | watch | ini | ffb <n>]\n"); rc = 2; }
    for (auto& d : g_devs)
        if (d.dev) { d.dev->Unacquire(); d.dev->Release(); }
    if (g_di) g_di->Release();
    return rc;
}
