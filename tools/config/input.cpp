#include "input.h"

#include <cstdio>

#include "i18n.h"

namespace gp4cfg {

std::string to_utf8(const char* ansi) {
    int n = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
    if (n <= 0) return ansi;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, ansi, -1, &w[0], n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(m > 0 ? m : 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], m, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

static std::string guid_str(const GUID& g) {
    char b[64];
    std::snprintf(b, sizeof b, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, g.Data2,
                  g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

double Controller::axis01(int a) const {
    if (a < 0 || a > 7 || !axis[a]) return 0.5;
    LONG v;
    switch (a) {
        case 0: v = st.lX; break;
        case 1: v = st.lY; break;
        case 2: v = st.lZ; break;
        case 3: v = st.lRx; break;
        case 4: v = st.lRy; break;
        case 5: v = st.lRz; break;
        case 6: v = st.rglSlider[0]; break;
        default: v = st.rglSlider[1]; break;
    }
    double d = v / 65535.0;
    return d < 0 ? 0 : d > 1 ? 1 : d;
}

bool Input::init(HWND hwnd, const std::string& dll) {
    hwnd_ = hwnd;
    std::string path = dll;
    if (path.empty()) {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);   // SysWOW64 for this 32-bit program
        path = std::string(sys) + "\\dinput8.dll";
    }
    HMODULE m = LoadLibraryA(path.c_str());
    typedef HRESULT(WINAPI * Create8)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    Create8 create = m ? (Create8)GetProcAddress(m, "DirectInput8Create") : nullptr;
    if (!create || FAILED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&di_, nullptr))) {
        error_ = strf(T("Could not open DirectInput (%s)"), path.c_str());
        di_ = nullptr;
        return false;
    }
    enumerate();
    return true;
}

static BOOL CALLBACK enum_dev(LPCDIDEVICEINSTANCEA i, LPVOID r) {
    auto* v = (std::vector<DIDEVICEINSTANCEA>*)r;
    v->push_back(*i);
    return DIENUM_CONTINUE;
}

static BOOL CALLBACK enum_obj(LPCDIDEVICEOBJECTINSTANCEA o, LPVOID r) {
    auto* c = (Controller*)r;
    const GUID* g[6] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis};
    for (int a = 0; a < 6; ++a)
        if (o->guidType == *g[a]) c->axis[a] = true;
    if (o->guidType == GUID_Slider) (c->axis[6] ? c->axis[7] : c->axis[6]) = true;
    return DIENUM_CONTINUE;
}

void Input::enumerate() {
    if (!di_) return;
    stop_test_effect();
    for (auto& c : list_)
        if (c.dev) { c.dev->Unacquire(); c.dev->Release(); }
    list_.clear();
    std::vector<DIDEVICEINSTANCEA> found;
    di_->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_dev, &found, DIEDFL_ATTACHEDONLY);
    for (size_t i = 0; i < found.size(); ++i) {
        Controller c;
        c.name = to_utf8(found[i].tszProductName);
        c.instance_name = to_utf8(found[i].tszInstanceName);
        c.vid = (uint16_t)(found[i].guidProduct.Data1 & 0xFFFF);   // HID product GUID: {PPPPVVVV-...}
        c.pid = (uint16_t)(found[i].guidProduct.Data1 >> 16);
        c.guid = guid_str(found[i].guidInstance);
        c.index = (int)i;
        if (SUCCEEDED(di_->CreateDevice(found[i].guidInstance, &c.dev, nullptr)) && c.dev) {
            c.dev->SetDataFormat(&c_dfDIJoystick2);
            DIPROPRANGE r{};
            r.diph.dwSize = sizeof r; r.diph.dwHeaderSize = sizeof r.diph; r.diph.dwHow = DIPH_DEVICE;
            r.lMin = 0; r.lMax = 65535;
            c.dev->SetProperty(DIPROP_RANGE, &r.diph);
            DIDEVCAPS caps{};
            caps.dwSize = sizeof caps;
            c.dev->GetCapabilities(&caps);
            c.ff = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
            c.buttons = (int)caps.dwButtons;
            c.povs = (int)caps.dwPOVs;
            c.dev->EnumObjects(enum_obj, &c, DIDFT_AXIS);
            c.dev->SetCooperativeLevel(hwnd_, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
            c.dev->Acquire();
        }
        for (auto& p : c.st.rgdwPOV) p = 0xFFFFFFFF;
        list_.push_back(c);
    }
    poll();
}

void Input::poll() {
    for (size_t i = 0; i < list_.size(); ++i) {
        Controller& c = list_[i];
        if (!c.dev) continue;
        c.dev->Poll();
        HRESULT hr = c.dev->GetDeviceState(sizeof c.st, &c.st);
        if (FAILED(hr)) {
            c.dev->Acquire();
            hr = c.dev->GetDeviceState(sizeof c.st, &c.st);
        }
        c.ok = SUCCEEDED(hr);
    }
}

void Input::stop_test_effect() {
    if (test_.fx) {
        DICONSTANTFORCE cf{0};
        (void)cf;
        test_.fx->Stop();
        test_.fx->Release();
        test_.fx = nullptr;
    }
    if (test_.device >= 0 && test_.device < (int)list_.size() && list_[test_.device].dev) {
        IDirectInputDevice8A* d = list_[test_.device].dev;
        d->Unacquire();
        d->SetCooperativeLevel(hwnd_, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        d->Acquire();
    }
}

static void set_force(IDirectInputEffect* fx, LONG mag) {
    DWORD axes[1] = {DIJOFS_X};
    LONG dir[1] = {0};
    DICONSTANTFORCE cf{mag};
    DIEFFECT e{};
    e.dwSize = sizeof e; e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS; e.dwDuration = INFINITE;
    e.dwGain = DI_FFNOMINALMAX; e.dwTriggerButton = DIEB_NOTRIGGER; e.cAxes = 1; e.rgdwAxes = axes;
    e.rglDirection = dir; e.cbTypeSpecificParams = sizeof cf; e.lpvTypeSpecificParams = &cf;
    fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS | DIEP_START);
}

void Input::start_ffb_test(int device, double now) {
    if (test_.state == FfbTest::Positive || test_.state == FfbTest::Negative) return;
    test_ = FfbTest();
    test_.device = device;
    if (device < 0 || device >= (int)list_.size() || !list_[device].dev || !list_[device].ff) {
        test_.state = FfbTest::Failed;
        test_.message = T("That controller has no force feedback.");
        return;
    }
    IDirectInputDevice8A* d = list_[device].dev;
    // Force feedback needs exclusive access (the game must not be running).
    d->Unacquire();
    if (FAILED(d->SetCooperativeLevel(hwnd_, DISCL_EXCLUSIVE | DISCL_BACKGROUND))) {
        test_.state = FfbTest::Failed;
        test_.message = T("Could not get exclusive use of the base. Is GP4 running?");
        stop_test_effect();
        return;
    }
    DIPROPDWORD p{};
    p.diph.dwSize = sizeof p; p.diph.dwHeaderSize = sizeof p.diph; p.diph.dwHow = DIPH_DEVICE;
    p.dwData = DIPROPAUTOCENTER_OFF;
    d->SetProperty(DIPROP_AUTOCENTER, &p.diph);
    d->Acquire();
    DWORD axes[1] = {DIJOFS_X};
    LONG dir[1] = {0};
    DICONSTANTFORCE cf{0};
    DIEFFECT e{};
    e.dwSize = sizeof e; e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS; e.dwDuration = INFINITE;
    e.dwGain = DI_FFNOMINALMAX; e.dwTriggerButton = DIEB_NOTRIGGER; e.cAxes = 1; e.rgdwAxes = axes;
    e.rglDirection = dir; e.cbTypeSpecificParams = sizeof cf; e.lpvTypeSpecificParams = &cf;
    if (FAILED(d->CreateEffect(GUID_ConstantForce, &e, &test_.fx, nullptr)) || !test_.fx) {
        test_.fx = nullptr;
        test_.state = FfbTest::Failed;
        test_.message = T("The base did not accept the force effect.");
        stop_test_effect();
        return;
    }
    poll();
    test_.x0 = list_[device].st.lX;
    set_force(test_.fx, 3000);
    test_.t0 = now;
    test_.state = FfbTest::Positive;
}

void Input::update_ffb_test(double now) {
    if (test_.state != FfbTest::Positive && test_.state != FfbTest::Negative) return;
    if (now - test_.t0 < 1.5) return;
    Controller& c = list_[test_.device];
    if (test_.state == FfbTest::Positive) {
        test_.x1 = c.st.lX;
        set_force(test_.fx, -3000);
        test_.t0 = now;
        test_.state = FfbTest::Negative;
        return;
    }
    test_.x2 = c.st.lX;
    set_force(test_.fx, 0);
    stop_test_effect();
    test_.state = FfbTest::Done;
    // DirectInput (and GP4): a positive magnitude pushes towards -X.
    if (test_.x2 > test_.x1 + 3000) {
        test_.result = +1;
        test_.message = T("Correct: a positive force turns the wheel left (the standard). Invert = no.");
    } else if (test_.x1 > test_.x2 + 3000) {
        test_.result = -1;
        test_.message = T("Your base works the opposite way to the standard: 'Invert the force' has been ticked.");
    } else {
        test_.result = 0;
        test_.message = T("The wheel barely moved (were you holding it?). Let go of it and repeat the test.");
    }
}

void Input::shutdown() {
    stop_test_effect();
    for (auto& c : list_)
        if (c.dev) { c.dev->Unacquire(); c.dev->Release(); c.dev = nullptr; }
    list_.clear();
    if (di_) di_->Release();
    di_ = nullptr;
}

}  // namespace gp4cfg
