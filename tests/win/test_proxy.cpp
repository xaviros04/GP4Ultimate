// End-to-end test of the dinput8 proxy. This program plays the part of GP4: it imports
// DirectInput8Create from dinput8.dll (the proxy, in the same folder), which chains to
// mock_dinput.dll (fake wheel + pedals). It also hosts a fake GP4 car table in memory so
// the telemetry FFB engine has something to find.
//
//   test_proxy telemetry   composite input + telemetry FFB
//   test_proxy game        game effects passed through to the wheel
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/car_data.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define CHECKV(c, v) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s  (value %ld)\n", __LINE__, #c, (long)(v)); } } while (0)

static std::string g_dir;

typedef void (*SetAxis)(int, int, LONG);
typedef void (*SetButton)(int, int, int);
typedef void (*SetPov)(int, int, DWORD);
typedef LONG (*GetL)();
typedef DWORD (*GetCoop)(int);
static SetAxis mock_set_axis;
static SetButton mock_set_button;
static SetPov mock_set_pov;
static GetL mock_force, mock_effects_alive, mock_effects_created, mock_last_axis_ofs, mock_last_naxes, mock_last_dir, mock_autocenter;
static GetCoop mock_coop;
static GetL mock_starts, mock_resets;
typedef void (*DevFn)(int);
static DevFn mock_lose_focus, mock_stop_all;

static void write_ini(const char* mode, bool hide_others = true) {
    std::ofstream f(g_dir + "\\gp4ultimate.ini");
    f << "[general]\nchain = mock_dinput.dll\nlog = 1\n"
         "[devices]\nwheel = name:\"Mock Wheel\"\npedals = vidpid:30B7:1001\n"
         "[virtual]\nffb = wheel\nhide_others = " << (hide_others ? 1 : 0) << "\n"
         "[axes]\nX = wheel.X\nY = pedals.Y invert\nRZ = pedals.RZ invert\nZ = pedals.Z\n"
         "[buttons]\nauto = wheel\n[povs]\n0 = wheel.pov0\n"
         "[ffb]\nmode = " << mode << "\nmaster_gain = 0.7\ngame_scale = 0.00006103515625\ngame_invert = 0\n"
         "game_effects_gain = 0.5\nshift_kick = 0\nimpact_gain = 0\n"
         "[telemetry]\nscan_heap = 1\ncar_pointer = 0\nrespect_game_pause = 0\n";
}

// Like GP4: a static pointer to the player's car, pause flags, the aligning term.
static uint32_t g_car_ptr;
static volatile uint8_t g_pause[3];
static volatile int32_t g_aligning;
static uint8_t g_wheel_rec[22 * 0xA8];   // GP4's per-car wheel record (surface, slides)

static void write_ini_gp4(bool aligning) {
    std::ofstream f(g_dir + "\\gp4ultimate.ini");
    char tel[256];
    std::snprintf(tel, sizeof tel, "car_pointer = 0x%08lx\npause_flags = 0x%08lx,0x%08lx,0x%08lx\naligning_address = 0x%08lx\nwheel_record = 0x%08lx\n",
                  (unsigned long)(uintptr_t)&g_car_ptr, (unsigned long)(uintptr_t)&g_pause[0],
                  (unsigned long)(uintptr_t)&g_pause[1], (unsigned long)(uintptr_t)&g_pause[2],
                  (unsigned long)(uintptr_t)&g_aligning, (unsigned long)(uintptr_t)g_wheel_rec);
    // Everything else at its defaults: that is what the user gets.
    f << "[general]\nchain = mock_dinput.dll\nlog = 1\n"
         "[devices]\nwheel = name:\"Mock Wheel\"\npedals = vidpid:30B7:1001\n"
         "[virtual]\nffb = wheel\n"
         "[axes]\nX = wheel.X\nY = pedals.Y invert\nRZ = pedals.RZ invert\nZ = pedals.Z\n"
         "[buttons]\nauto = wheel\n"
         "[ffb]\nshift_kick = 0\nimpact_gain = 0\nlog_csv = 1\n" << (aligning ? "game_signal = aligning\n" : "") <<
         "[telemetry]\n" << tel;
}

static bool load_mock() {
    HMODULE m = GetModuleHandleA("mock_dinput.dll");
    if (!m) return false;
    mock_set_axis = (SetAxis)GetProcAddress(m, "mock_set_axis");
    mock_set_button = (SetButton)GetProcAddress(m, "mock_set_button");
    mock_set_pov = (SetPov)GetProcAddress(m, "mock_set_pov");
    mock_force = (GetL)GetProcAddress(m, "mock_force");
    mock_effects_alive = (GetL)GetProcAddress(m, "mock_effects_alive");
    mock_effects_created = (GetL)GetProcAddress(m, "mock_effects_created");
    mock_last_axis_ofs = (GetL)GetProcAddress(m, "mock_last_axis_ofs");
    mock_last_naxes = (GetL)GetProcAddress(m, "mock_last_naxes");
    mock_last_dir = (GetL)GetProcAddress(m, "mock_last_dir");
    mock_autocenter = (GetL)GetProcAddress(m, "mock_autocenter");
    mock_coop = (GetCoop)GetProcAddress(m, "mock_coop");
    mock_starts = (GetL)GetProcAddress(m, "mock_starts");
    mock_resets = (GetL)GetProcAddress(m, "mock_resets");
    mock_lose_focus = (DevFn)GetProcAddress(m, "mock_lose_focus");
    mock_stop_all = (DevFn)GetProcAddress(m, "mock_stop_all");
    return mock_set_axis && mock_force && mock_coop && mock_starts && mock_lose_focus && mock_stop_all;
}

static std::string read_log() {
    std::ifstream f(g_dir + "\\gp4ultimate.log");
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

struct EnumOut { std::vector<DIDEVICEINSTANCEA> list; };
static BOOL CALLBACK enum_cb(LPCDIDEVICEINSTANCEA d, LPVOID r) { ((EnumOut*)r)->list.push_back(*d); return DIENUM_CONTINUE; }
static int g_objs, g_ff_objs;
static BOOL CALLBACK obj_cb(LPCDIDEVICEOBJECTINSTANCEA, LPVOID r) { ++*(int*)r; return DIENUM_CONTINUE; }

static void set_range(IDirectInputDevice8A* d, DWORD how, DWORD obj, LONG lo, LONG hi) {
    DIPROPRANGE r{};
    r.diph.dwSize = sizeof r; r.diph.dwHeaderSize = sizeof r.diph; r.diph.dwHow = how; r.diph.dwObj = obj;
    r.lMin = lo; r.lMax = hi;
    d->SetProperty(DIPROP_RANGE, &r.diph);
}

static void set_dword(IDirectInputDevice8A* d, REFGUID p, DWORD how, DWORD obj, DWORD v) {
    DIPROPDWORD x{};
    x.diph.dwSize = sizeof x; x.diph.dwHeaderSize = sizeof x.diph; x.diph.dwHow = how; x.diph.dwObj = obj;
    x.dwData = v;
    d->SetProperty(p, &x.diph);
}

static IDirectInputDevice8A* open_virtual(IDirectInput8A* di, size_t expected = 1) {
    EnumOut e;
    di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_cb, &e, DIEDFL_ATTACHEDONLY);
    // Mapped mocks always hidden; the unmapped "Mock vJoy" only shows with hide_others = 0.
    CHECKV(e.list.size() == expected, e.list.size());
    if (expected == 2 && e.list.size() == 2) CHECK(std::strcmp(e.list[1].tszProductName, "Mock vJoy") == 0);
    if (e.list.empty()) return nullptr;
    CHECK(std::strcmp(e.list[0].tszProductName, "GP4 Composite Wheel") == 0);
    EnumOut ff;
    di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_cb, &ff, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    CHECK(!ff.list.empty() && ff.list[0].guidInstance == e.list[0].guidInstance);
    IDirectInputDevice8A* dev = nullptr;
    CHECK(SUCCEEDED(di->CreateDevice(e.list[0].guidInstance, &dev, nullptr)) && dev);
    return dev;
}

// --- fake GP4 car table -------------------------------------------------------------
static uint8_t* g_cars;
static const int kPlayer = 4;
static uint8_t* player() { return g_cars + kPlayer * gp4u::car::kSize; }

static void make_cars() {
    using namespace gp4u::car;
    g_cars = (uint8_t*)VirtualAlloc(nullptr, 22 * kSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int i = 0; i < 22; ++i) {
        uint8_t* p = g_cars + i * kSize;
        p[kId] = (uint8_t)(i == kPlayer ? 0x80 | i : i);
        p[0x202] = (uint8_t)i;   // slot in the car table (indexes the wheel record)
        uint16_t w = 600;
        std::memcpy(p + kWeight, &w, 2);
        p[kNumGears] = 7;
        p[kCurrentGear] = 4;
        p[kTeamIndex] = (uint8_t)(i / 2);
        p[kFrontWing] = 10;
        p[kRearWing] = 12;
        int32_t speed = (int32_t)(50.0 / kFootToMetre * kFeetPerSecondRaw);   // 50 m/s
        std::memcpy(p + kSpeed, &speed, 4);
    }
    uint16_t ff = 8192;   // 0.5 with game_scale = 1/16384
    std::memcpy(player() + gp4u::car::kForceFeedback, &ff, 2);
}

static LONG settle_force(int ms) {
    Sleep(ms);
    return mock_force();
}

static void run_telemetry() {
    write_ini("telemetry");
    make_cars();
    IDirectInput8A* di = nullptr;
    HRESULT hr = DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&di, nullptr);
    CHECK(SUCCEEDED(hr) && di);
    if (!di) return;
    CHECK(load_mock());
    if (!mock_set_axis) return;
    IDirectInputDevice8A* dev = open_virtual(di);
    if (!dev) return;

    DIDEVCAPS caps{};
    caps.dwSize = sizeof caps;
    dev->GetCapabilities(&caps);
    CHECKV(caps.dwAxes == 4, caps.dwAxes);
    CHECKV(caps.dwButtons == 10, caps.dwButtons);
    CHECKV(caps.dwPOVs == 1, caps.dwPOVs);
    CHECK(caps.dwFlags & DIDC_FORCEFEEDBACK);
    g_objs = 0;
    dev->EnumObjects(obj_cb, &g_objs, DIDFT_ALL);
    CHECKV(g_objs == 15, g_objs);
    g_ff_objs = 0;
    dev->EnumObjects(obj_cb, &g_ff_objs, DIDFT_AXIS | DIDFT_FFACTUATOR);
    CHECKV(g_ff_objs == 1, g_ff_objs);

    // GP4-style setup: DIJOYSTATE format, ranges, cooperative level.
    CHECK(SUCCEEDED(dev->SetDataFormat(&c_dfDIJoystick)));
    set_range(dev, DIPH_DEVICE, 0, 0, 1000);
    set_range(dev, DIPH_BYOFFSET, DIJOFS_X, -1000, 1000);
    DIPROPRANGE r{};
    r.diph.dwSize = sizeof r; r.diph.dwHeaderSize = sizeof r.diph; r.diph.dwHow = DIPH_BYOFFSET; r.diph.dwObj = DIJOFS_X;
    CHECK(SUCCEEDED(dev->GetProperty(DIPROP_RANGE, &r.diph)) && r.lMin == -1000 && r.lMax == 1000);
    DIJOYSTATE js{};
    CHECK(dev->GetDeviceState(sizeof js, &js) == DIERR_NOTACQUIRED);
    // GP4.exe 0x5061ea: CALIBRATIONMODE = RAW, and it drops the device if that fails.
    set_dword(dev, DIPROP_CALIBRATIONMODE, DIPH_DEVICE, 0, DIPROPCALIBRATIONMODE_RAW);
    DIPROPDWORD cm{};
    cm.diph.dwSize = sizeof cm; cm.diph.dwHeaderSize = sizeof cm.diph; cm.diph.dwHow = DIPH_DEVICE;
    CHECK(SUCCEEDED(dev->GetProperty(DIPROP_CALIBRATIONMODE, &cm.diph)) && cm.dwData == DIPROPCALIBRATIONMODE_RAW);
    // GP4 asks for DISCL_EXCLUSIVE | DISCL_FOREGROUND (0x505fdf push 5).
    CHECK(SUCCEEDED(dev->SetCooperativeLevel((HWND)0x1234, DISCL_EXCLUSIVE | DISCL_FOREGROUND)));
    CHECKV(mock_coop(0) == (DISCL_EXCLUSIVE | DISCL_FOREGROUND), mock_coop(0));   // FFB wheel exclusive
    CHECKV(mock_coop(1) == (DISCL_NONEXCLUSIVE | DISCL_BACKGROUND), mock_coop(1));
    CHECKV(mock_autocenter() == DIPROPAUTOCENTER_OFF, mock_autocenter());
    CHECK(SUCCEEDED(dev->Acquire()));

    // Composite state.
    mock_set_axis(0, 0, 49151);     // wheel 75% right
    mock_set_axis(1, 1, 0);         // throttle fully pressed (inverted axis)
    mock_set_axis(1, 5, 65535);     // brake released (inverted)
    mock_set_axis(1, 2, 16384);     // clutch 25%
    mock_set_button(0, 3, 1);
    mock_set_pov(0, 0, 9000);
    CHECK(SUCCEEDED(dev->GetDeviceState(sizeof js, &js)));
    CHECKV(std::labs(js.lX - 500) <= 1, js.lX);
    CHECKV(js.lY == 1000, js.lY);
    CHECKV(js.lRz == 0, js.lRz);
    CHECKV(std::labs(js.lZ - 250) <= 1, js.lZ);
    CHECKV(js.lRx == 0, js.lRx);                        // unbound axis
    CHECK(js.rgbButtons[3] == 0x80 && js.rgbButtons[4] == 0);
    CHECKV(js.rgdwPOV[0] == 9000, js.rgdwPOV[0]);
    CHECKV(js.rgdwPOV[1] == 0xFFFFFFFF, js.rgdwPOV[1]); // unbound POV reads centred
    CHECK(dev->GetDeviceState(sizeof(DIJOYSTATE2), &js) == DIERR_INVALIDPARAM);   // wrong size

    // DirectInput deadzone semantics on top of ours.
    set_dword(dev, DIPROP_DEADZONE, DIPH_BYOFFSET, DIJOFS_X, 6000);
    dev->GetDeviceState(sizeof js, &js);
    CHECKV(js.lX == 0, js.lX);
    set_dword(dev, DIPROP_DEADZONE, DIPH_BYOFFSET, DIJOFS_X, 0);

    // Buffered data.
    dev->Unacquire();
    set_dword(dev, DIPROP_BUFFERSIZE, DIPH_DEVICE, 0, 32);
    dev->Acquire();
    DWORD n = INFINITE;
    dev->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), nullptr, &n, 0);   // flush
    mock_set_button(0, 3, 0);
    DIDEVICEOBJECTDATA ev[8];
    n = 8;
    CHECK(SUCCEEDED(dev->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), ev, &n, 0)));
    CHECKV(n == 1, n);
    if (n >= 1) CHECK(ev[0].dwOfs == (DWORD)DIJOFS_BUTTON(3) && ev[0].dwData == 0);

    // Game effects in telemetry mode are swallowed (null effects).
    LONG created = mock_effects_created();
    IDirectInputEffect* gfx = nullptr;
    CHECK(SUCCEEDED(dev->CreateEffect(GUID_ConstantForce, nullptr, &gfx, nullptr)) && gfx);
    if (gfx) gfx->Release();

    // Telemetry FFB: the engine must find the car table and apply GP4's FF value.
    mock_set_axis(0, 0, 32768);
    LONG f = 0;
    for (int i = 0; i < 40; ++i) {   // up to 8 s (first scan + low-pass)
        f = settle_force(200);
        if (std::labs(f - 3500) <= 250) break;
    }
    CHECKV(std::labs(f - 3500) <= 250, f);   // 0.7 * 0.5 * 10000
    CHECKV(mock_effects_created() == created + 1, mock_effects_created());   // only the engine's effect

    // Steering correlation: the player's steerRaw follows the wheel -> confirmed.
    for (int i = 0; i < 300; ++i) {
        double s = 0.3 * std::sin(i * 2 * 3.14159 / 50.0);
        mock_set_axis(0, 0, (LONG)(32768 + s * 32767));
        uint16_t raw = (uint16_t)(32768 + s * 20000);
        std::memcpy(player() + gp4u::car::kSteerRaw, &raw, 2);
        Sleep(20);
    }
    CHECK(read_log().find("confirmed as the player car") != std::string::npos);

    // Car disappears (end of session): fall back to the gentle centring spring.
    mock_set_axis(0, 0, 49151);
    player()[gp4u::car::kId] = 0;
    f = settle_force(2000);
    // Physical spring -0.15 * 0.5 (pushes left) * 0.7; as a DirectInput magnitude that is
    // +525 (positive pushes towards -X).
    CHECKV(std::labs(f - 525) <= 80, f);
    CHECK(read_log().find("dropping") != std::string::npos);

    dev->Unacquire();
    dev->Release();   // stops the engine: zero force, effect released
    CHECKV(mock_force() == 0, mock_force());
    CHECKV(mock_effects_alive() == 0, mock_effects_alive());
    di->Release();
}

static void run_game() {
    write_ini("game", false);
    IDirectInput8A* di = nullptr;
    DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&di, nullptr);
    CHECK(di && load_mock());
    if (!di || !mock_force) return;
    IDirectInputDevice8A* dev = open_virtual(di, 2);
    if (!dev) return;
    dev->SetDataFormat(&c_dfDIJoystick);
    dev->SetCooperativeLevel((HWND)0x1234, DISCL_EXCLUSIVE | DISCL_FOREGROUND);
    dev->Acquire();
    Sleep(300);
    CHECKV(mock_effects_created() == 0, mock_effects_created());   // no engine in game mode

    // A 2-axis polar constant force pointing to -X, as an older game might create it.
    DWORD axes[2] = {DIJOFS_X, DIJOFS_Y};
    LONG dir[2] = {27000, 0};
    DICONSTANTFORCE cf{5000};
    DIEFFECT e{};
    e.dwSize = sizeof e; e.dwFlags = DIEFF_POLAR | DIEFF_OBJECTOFFSETS; e.dwDuration = INFINITE;
    e.dwGain = 8000; e.dwTriggerButton = DIEB_NOTRIGGER; e.cAxes = 2; e.rgdwAxes = axes; e.rglDirection = dir;
    e.cbTypeSpecificParams = sizeof cf; e.lpvTypeSpecificParams = &cf;
    IDirectInputEffect* fx = nullptr;
    CHECK(SUCCEEDED(dev->CreateEffect(GUID_ConstantForce, &e, &fx, nullptr)) && fx);
    if (fx) CHECK(SUCCEEDED(fx->Start(1, 0)));
    CHECKV(mock_last_naxes() == 1, mock_last_naxes());
    CHECKV(mock_last_axis_ofs() == 0, mock_last_axis_ofs());   // wheel X in DIJOYSTATE2
    CHECKV(mock_last_dir() == -1, mock_last_dir());
    CHECKV(mock_force() == 2000, mock_force());                // 5000 * (8000 * 0.5) / 10000
    if (fx) {
        cf.lMagnitude = -3000;
        fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS);
        CHECKV(mock_force() == -1200, mock_force());
        fx->Release();
    }
    CHECKV(mock_effects_alive() == 0, mock_effects_alive());
    dev->Release();
    di->Release();
}

// GP4 path with defaults: pointer -> car, GP4's scale/sign, pause handling.
static void run_gp4(bool aligning) {
    write_ini_gp4(aligning);
    make_cars();
    g_car_ptr = (uint32_t)(uintptr_t)player();
    player()[gp4u::car::kId] = 0x05;   // via the pointer the player bit is not required
    // ...nor the plausibility check (first real run: GP4's record failed it).
    player()[gp4u::car::kNumGears] = 0;
    uint16_t odd_weight = 0;
    std::memcpy(player() + gp4u::car::kWeight, &odd_weight, 2);
    player()[gp4u::car::kFfScaling] = 50;
    int16_t raw = 2000;
    std::memcpy(player() + gp4u::car::kForceFeedback, &raw, 2);
    g_aligning = 4000;                   // * 50 / 100 = 2000, same as the total
    IDirectInput8A* di = nullptr;
    DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&di, nullptr);
    CHECK(di && load_mock());
    if (!di || !mock_force) return;
    IDirectInputDevice8A* dev = open_virtual(di);
    if (!dev) return;
    dev->SetDataFormat(&c_dfDIJoystick);
    dev->SetCooperativeLevel((HWND)0x1234, DISCL_EXCLUSIVE | DISCL_BACKGROUND);   // what GP4 asks for
    dev->Acquire();
    mock_set_axis(0, 0, 32768);
    // GP4 itself would send -1.5 * 2000 / 10000 = -0.3; times the default master_gain 0.55 = -0.165.
    LONG f = settle_force(1500);
    CHECKV(std::labs(f - (-1650)) <= 120, f);
    CHECK(read_log().find(aligning ? "aligning term" : "total (+0x5E)") != std::string::npos);
    CHECK(read_log().find("used anyway: trusted address") != std::string::npos);
    // Pause flag set: GP4 zeroes the force; we fall back to the centring spring (wheel centred -> ~0).
    g_pause[1] = 1;
    f = settle_force(800);
    CHECKV(std::labs(f) <= 60, f);
    g_pause[1] = 0;
    f = settle_force(800);
    CHECKV(std::labs(f - (-1650)) <= 120, f);
    // In-game report: "felt force leaving the pits, then the wheel went dead". The window
    // lost focus for a moment; re-acquiring an FF wheel exclusively resets it and stops our
    // effect. The engine must notice and get the force back.
    LONG resets = mock_resets();
    mock_lose_focus(0);
    f = settle_force(600);
    CHECKV(mock_resets() > resets, mock_resets());
    CHECKV(std::labs(f - (-1650)) <= 120, f);
    CHECK(read_log().find("re-acquired") != std::string::npos);
    // Something outside the mod stops the effect while we stay acquired.
    mock_stop_all(0);
    f = settle_force(600);
    CHECKV(std::labs(f - (-1650)) <= 120, f);
    CHECK(read_log().find("found stopped") != std::string::npos);
    // Player's front-left wheel on a kerb for a moment: vibration (checked in the CSV).
    int32_t kerb = 1;
    std::memcpy(g_wheel_rec + kPlayer * 0xA8 + 0x68 + 2 * 4, &kerb, 4);
    Sleep(400);
    kerb = 0;
    std::memcpy(g_wheel_rec + kPlayer * 0xA8 + 0x68 + 2 * 4, &kerb, 4);
    // No player car (pointer 0): no force from the car.
    g_car_ptr = 0;
    f = settle_force(800);
    CHECKV(std::labs(f) <= 60, f);
    dev->Unacquire();
    dev->Release();
    CHECKV(mock_force() == 0, mock_force());
    // The CSV (calibration data): consistent columns, GP4's aligning term logged in
    // every mode (2000 = 4000 * ffScaling 50 / 100), the own model in shadow mode.
    {
        std::ifstream csv(g_dir + "\\gp4ultimate_ffb.csv");
        std::string header, line, last_valid;
        std::getline(csv, header);
        while (std::getline(csv, line))
            if (line.size() > 2 && line.find(",1,") == line.find(',')) last_valid = line;
        auto cols = [](const std::string& l) { return std::count(l.begin(), l.end(), ',') + 1; };
        CHECK(header.find(",game_align,model_raw") != std::string::npos);
        CHECK(!last_valid.empty() && cols(last_valid) == cols(header));
        auto field = [](const std::string& l, int idx) {
            size_t p = 0;
            for (int i = 0; i < idx && p != std::string::npos; ++i) p = l.find(',', p) + 1;
            return p == std::string::npos ? std::string() : l.substr(p, l.find(',', p) - p);
        };
        int col = -1;
        for (int i = 0; col < 0 && i < 64; ++i)
            if (field(header, i) == "game_align") col = i;
        CHECK(col > 0 && std::atoi(field(last_valid, col).c_str()) == 2000);
        CHECK(header.find(",game_hz") != std::string::npos);
        // The kerb moment: surf_fl = 1 rows with a texture, read from the player's slot.
        int c_surf = -1, c_tex = -1;
        for (int i = 0; i < 64; ++i) {
            if (field(header, i) == "surf_fl") c_surf = i;
            if (field(header, i) == "c_texture") c_tex = i;
        }
        std::ifstream csv2(g_dir + "\\gp4ultimate_ffb.csv");
        std::string l2;
        std::getline(csv2, l2);
        int kerb_rows = 0, vib_rows = 0;
        while (std::getline(csv2, l2)) {
            if (c_surf < 0 || field(l2, c_surf) != "1") continue;
            ++kerb_rows;
            if (std::fabs(std::atof(field(l2, c_tex).c_str())) > 0.001) ++vib_rows;
        }
        CHECKV(kerb_rows > 25, kerb_rows);   // 0.4 s at 100 rows/s
        CHECKV(vib_rows > kerb_rows / 2, vib_rows);
    }
    di->Release();
}

int main(int argc, char** argv) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of("\\/"));
    std::string mode = argc > 1 ? argv[1] : "telemetry";
    if (mode == "game") run_game();
    else if (mode == "gp4") run_gp4(false);
    else if (mode == "aligning") run_gp4(true);
    else run_telemetry();
    std::printf("[%s] %d passed, %d failed\n", mode.c_str(), g_pass, g_fail);
    return g_fail ? 1 : 0;
}
