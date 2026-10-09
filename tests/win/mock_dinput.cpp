// Fake DirectInput 8 for end-to-end tests of the proxy: a force-feedback wheel and a
// pedal set whose state the test drives through exported functions. Loaded by the proxy
// via [general] chain = mock_dinput.dll, so the proxy's own device manager AND the game's
// pass-through enumeration both see these devices.
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>

#include <cstring>
#include <vector>

namespace {

struct DevSpec {
    const char* name;
    GUID instance;
    DWORD vidpid;
    bool axes[8];
    int buttons;
    int povs;
    bool ff;
};

DevSpec g_spec[3] = {
    {"Mock Wheel", {0x11111111, 1, 1, {1, 2, 3, 4, 5, 6, 7, 8}}, (DWORD)MAKELONG(0x046D, 0xC24F), {1, 0, 0, 0, 0, 0, 0, 0}, 10, 1, true},
    {"Mock Pedals", {0x22222222, 2, 2, {1, 2, 3, 4, 5, 6, 7, 8}}, (DWORD)MAKELONG(0x30B7, 0x1001), {0, 1, 1, 0, 0, 1, 0, 0}, 0, 0, false},
    // Not in the mapping, with force feedback, like the vJoy device found on a real rig.
    {"Mock vJoy", {0x33333333, 3, 3, {1, 2, 3, 4, 5, 6, 7, 8}}, (DWORD)MAKELONG(0x1234, 0xBEAD), {1, 1, 1, 1, 1, 1, 1, 1}, 128, 4, true},
};

struct DevState {
    LONG axis[8] = {32768, 0, 0, 0, 0, 0, 0, 0};
    BYTE buttons[128] = {};
    DWORD pov[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    DWORD coop = 0;
    bool acquired = false;
};
DevState g_state[3];

volatile LONG g_force = 0;          // last constant-force magnitude * gain / 10000
volatile LONG g_effects_alive = 0;
volatile LONG g_effects_created = 0;
volatile LONG g_last_axis_ofs = -1;
volatile LONG g_last_dir = 0;
volatile LONG g_last_naxes = 0;
volatile LONG g_autocenter = -1;
volatile LONG g_starts = 0;          // Start() calls + SetParameters(DIEP_START)
volatile LONG g_resets = 0;          // device resets caused by an exclusive (re)acquire
CRITICAL_SECTION g_cs;
class MockEffect;
std::vector<MockEffect*> g_live;     // guarded by g_cs

class MockEffect : public IDirectInputEffect {
public:
    LONG ref = 1;
    GUID type;
    LONG mag = 0;
    DWORD gain = 10000;
    int dev;
    // Like real DirectInput, a created effect is NOT playing until Start() or DIEP_START, and
    // an exclusive (re)acquire of its device resets the device, stopping every effect.
    bool playing = false;
    MockEffect(REFGUID t, int d) : type(t), dev(d) {
        InterlockedIncrement(&g_effects_alive); InterlockedIncrement(&g_effects_created);
        EnterCriticalSection(&g_cs); g_live.push_back(this); LeaveCriticalSection(&g_cs);
    }
    virtual ~MockEffect() {
        InterlockedDecrement(&g_effects_alive);
        EnterCriticalSection(&g_cs);
        for (size_t i = 0; i < g_live.size(); ++i) if (g_live[i] == this) { g_live.erase(g_live.begin() + i); break; }
        LeaveCriticalSection(&g_cs);
    }
    void publish() { g_force = playing ? (LONG)((long long)mag * gain / 10000) : 0; }
    void apply(LPCDIEFFECT e, DWORD flags) {
        if (!e) return;
        if ((flags & DIEP_GAIN) || flags == 0xFFFFFFFF) gain = e->dwGain;
        if ((flags & DIEP_AXES) && e->cAxes && e->rgdwAxes) { g_last_axis_ofs = (LONG)e->rgdwAxes[0]; g_last_naxes = (LONG)e->cAxes; }
        if ((flags & DIEP_DIRECTION) && e->cAxes && e->rglDirection) g_last_dir = e->rglDirection[0];
        if ((flags & DIEP_TYPESPECIFICPARAMS) && e->lpvTypeSpecificParams && type == GUID_ConstantForce)
            mag = ((const DICONSTANTFORCE*)e->lpvTypeSpecificParams)->lMagnitude;
        if (flags != 0xFFFFFFFF && (flags & DIEP_START)) { playing = true; InterlockedIncrement(&g_starts); }
        publish();
    }
    STDMETHOD(QueryInterface)(REFIID, void** p) override { *p = this; AddRef(); return S_OK; }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    STDMETHOD(GetEffectGuid)(LPGUID g) override { *g = type; return DI_OK; }
    STDMETHOD(GetParameters)(LPDIEFFECT, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT e, DWORD f) noexcept override;
    STDMETHOD(Start)(DWORD, DWORD) override { playing = true; InterlockedIncrement(&g_starts); publish(); return DI_OK; }
    STDMETHOD(Stop)() override { playing = false; publish(); return DI_OK; }
    STDMETHOD(GetEffectStatus)(LPDWORD s) override { *s = playing ? DIEGES_PLAYING : 0; return DI_OK; }
    STDMETHOD(Download)() override { return DI_OK; }
    STDMETHOD(Unload)() override { return DI_OK; }
    STDMETHOD(Escape)(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
};

class MockDevice : public IDirectInputDevice8A {
public:
    LONG ref = 1;
    int id;
    explicit MockDevice(int i) : id(i) {}
    virtual ~MockDevice() = default;
    const DevSpec& spec() const { return g_spec[id]; }
    DevState& st() const { return g_state[id]; }

    STDMETHOD(QueryInterface)(REFIID, void** p) override { *p = this; AddRef(); return S_OK; }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    STDMETHOD(GetCapabilities)(LPDIDEVCAPS c) override {
        DWORD sz = c->dwSize;
        std::memset(c, 0, sz);
        c->dwSize = sz;
        c->dwFlags = DIDC_ATTACHED | (spec().ff ? DIDC_FORCEFEEDBACK : 0);
        c->dwDevType = DI8DEVTYPE_DRIVING;
        for (bool a : spec().axes) c->dwAxes += a;
        c->dwButtons = spec().buttons;
        c->dwPOVs = spec().povs;
        return DI_OK;
    }
    STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD) override {
        const GUID* g[8] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider, &GUID_Slider};
        int inst = 0;
        for (int a = 0; a < 8; ++a) {
            if (!spec().axes[a]) continue;
            DIDEVICEOBJECTINSTANCEA o{};
            o.dwSize = sizeof o;
            o.guidType = *g[a];
            o.dwOfs = a < 6 ? a * 4 : 24 + (a - 6) * 4;
            o.dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(inst++);
            if (cb(&o, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }
    STDMETHOD(GetProperty)(REFGUID, LPDIPROPHEADER) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(SetProperty)(REFGUID g, LPCDIPROPHEADER h) override {
        const GUID* volatile pg = &g;
        const GUID* volatile pa = &DIPROP_AUTOCENTER;
        if ((uintptr_t)pg == (uintptr_t)pa) g_autocenter = (LONG)((const DIPROPDWORD*)h)->dwData;
        return DI_OK;
    }
    STDMETHOD(Acquire)() override {
        if (st().acquired) return S_FALSE;
        if (spec().ff && (st().coop & DISCL_EXCLUSIVE)) {   // DirectInput resets an FF device on exclusive acquire
            EnterCriticalSection(&g_cs);
            for (MockEffect* fx : g_live) if (fx->dev == id) { fx->playing = false; fx->publish(); }
            LeaveCriticalSection(&g_cs);
            InterlockedIncrement(&g_resets);
        }
        st().acquired = true;
        return DI_OK;
    }
    STDMETHOD(Unacquire)() override { st().acquired = false; return DI_OK; }
    STDMETHOD(GetDeviceState)(DWORD cb, LPVOID p) override {
        if (!st().acquired) return DIERR_NOTACQUIRED;
        if (cb != sizeof(DIJOYSTATE2)) return DIERR_INVALIDPARAM;
        EnterCriticalSection(&g_cs);
        DIJOYSTATE2 s{};
        s.lX = st().axis[0]; s.lY = st().axis[1]; s.lZ = st().axis[2];
        s.lRx = st().axis[3]; s.lRy = st().axis[4]; s.lRz = st().axis[5];
        s.rglSlider[0] = st().axis[6]; s.rglSlider[1] = st().axis[7];
        std::memcpy(s.rgbButtons, st().buttons, 128);
        std::memcpy(s.rgdwPOV, st().pov, sizeof s.rgdwPOV);
        LeaveCriticalSection(&g_cs);
        std::memcpy(p, &s, sizeof s);
        return DI_OK;
    }
    STDMETHOD(GetDeviceData)(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_NOTBUFFERED; }
    STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT f) override { return f->dwDataSize == sizeof(DIJOYSTATE2) ? DI_OK : DIERR_INVALIDPARAM; }
    STDMETHOD(SetEventNotification)(HANDLE) override { return DI_OK; }
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD f) override { st().coop = f; return DI_OK; }
    STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA d) override {
        DWORD sz = d->dwSize;
        std::memset(d, 0, sz);
        d->dwSize = sz;
        d->guidInstance = spec().instance;
        d->guidFFDriver = {0x33333333, 3, 3, {0}};
        lstrcpyA(d->tszProductName, spec().name);
        return DI_OK;
    }
    STDMETHOD(RunControlPanel)(HWND, DWORD) override { return DI_OK; }
    STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    STDMETHOD(CreateEffect)(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* out, LPUNKNOWN) override {
        if (!spec().ff) return DIERR_UNSUPPORTED;
        if (!(st().coop & DISCL_EXCLUSIVE)) return DIERR_NOTEXCLUSIVEACQUIRED;
        auto* fx = new MockEffect(g, id);
        fx->apply(e, 0xFFFFFFFF);
        *out = fx;
        return DI_OK;
    }
    STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD) override {
        if (!spec().ff) return DI_OK;
        DIEFFECTINFOA i{};
        i.dwSize = sizeof i;
        i.guid = GUID_ConstantForce;
        i.dwEffType = DIEFT_CONSTANTFORCE;
        lstrcpyA(i.tszName, "Constant Force");
        cb(&i, ref);
        i.guid = GUID_Spring;
        i.dwEffType = DIEFT_CONDITION;
        lstrcpyA(i.tszName, "Spring");
        cb(&i, ref);
        return DI_OK;
    }
    STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOA i, REFGUID g) override {
        i->guid = g;
        lstrcpyA(i->tszName, "Constant Force");
        return DI_OK;
    }
    STDMETHOD(GetForceFeedbackState)(LPDWORD s) override { *s = DIGFFS_POWERON; return DI_OK; }
    STDMETHOD(SendForceFeedbackCommand)(DWORD) override { return DI_OK; }
    STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
    STDMETHOD(Escape)(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(Poll)() override { return st().acquired ? DI_OK : DIERR_NOTACQUIRED; }
    STDMETHOD(SendDeviceData)(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DI_OK; }
    STDMETHOD(EnumEffectsInFile)(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(WriteEffectToFile)(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(BuildActionMap)(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(SetActionMap)(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(GetImageInfo)(LPDIDEVICEIMAGEINFOHEADERA) override { return DIERR_UNSUPPORTED; }
};

// Parameters are always stored; without exclusive acquisition they are not downloaded.
HRESULT STDMETHODCALLTYPE MockEffect::SetParameters(LPCDIEFFECT e, DWORD f) noexcept {
    apply(e, f);
    return g_state[dev].acquired ? DI_OK : DI_DOWNLOADSKIPPED;
}

class MockDI : public IDirectInput8A {
public:
    LONG ref = 1;
    virtual ~MockDI() = default;
    STDMETHOD(QueryInterface)(REFIID, void** p) override { *p = this; AddRef(); return S_OK; }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    STDMETHOD(CreateDevice)(REFGUID g, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN) override {
        for (int i = 0; i < 3; ++i)
            if (g == g_spec[i].instance) { *out = new MockDevice(i); return DI_OK; }
        return DIERR_DEVICENOTREG;
    }
    STDMETHOD(EnumDevices)(DWORD, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD) override {
        for (auto& s : g_spec) {
            DIDEVICEINSTANCEA d{};
            d.dwSize = sizeof d;
            d.guidInstance = s.instance;
            d.guidProduct = {s.vidpid, 0, 0, {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'}};
            d.dwDevType = DI8DEVTYPE_DRIVING;
            lstrcpyA(d.tszInstanceName, s.name);
            lstrcpyA(d.tszProductName, s.name);
            if (cb(&d, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }
    STDMETHOD(GetDeviceStatus)(REFGUID) override { return DI_OK; }
    STDMETHOD(RunControlPanel)(HWND, DWORD) override { return DI_OK; }
    STDMETHOD(Initialize)(HINSTANCE, DWORD) override { return DI_OK; }
    STDMETHOD(FindDevice)(REFGUID, LPCSTR, LPGUID) override { return DIERR_DEVICENOTREG; }
    STDMETHOD(EnumDevicesBySemantics)(LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID, DWORD) override { return DI_OK; }
    STDMETHOD(ConfigureDevices)(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSA, DWORD, LPVOID) override { return DI_OK; }
};

}  // namespace

extern "C" {
HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD, REFIID riid, LPVOID* out, LPUNKNOWN) {
    if (riid != IID_IDirectInput8A) return DIERR_NOINTERFACE;
    *out = new MockDI();
    return DI_OK;
}
void mock_set_axis(int dev, int axis, LONG v) { EnterCriticalSection(&g_cs); g_state[dev].axis[axis] = v; LeaveCriticalSection(&g_cs); }
void mock_set_button(int dev, int b, int down) { EnterCriticalSection(&g_cs); g_state[dev].buttons[b] = down ? 0x80 : 0; LeaveCriticalSection(&g_cs); }
void mock_set_pov(int dev, int p, DWORD v) { EnterCriticalSection(&g_cs); g_state[dev].pov[p] = v; LeaveCriticalSection(&g_cs); }
LONG mock_force() { return g_force; }
LONG mock_effects_alive() { return g_effects_alive; }
LONG mock_effects_created() { return g_effects_created; }
LONG mock_last_axis_ofs() { return g_last_axis_ofs; }
LONG mock_last_naxes() { return g_last_naxes; }
LONG mock_last_dir() { return g_last_dir; }
LONG mock_autocenter() { return g_autocenter; }
DWORD mock_coop(int dev) { return g_state[dev].coop; }
LONG mock_starts() { return g_starts; }
LONG mock_resets() { return g_resets; }
// The game window loses focus: DirectInput silently unacquires a foreground device.
void mock_lose_focus(int dev) { g_state[dev].acquired = false; }
// Something else (driver tool, another app) stops every effect while we stay acquired.
void mock_stop_all(int dev) {
    EnterCriticalSection(&g_cs);
    for (MockEffect* fx : g_live) if (fx->dev == dev) { fx->playing = false; fx->publish(); }
    LeaveCriticalSection(&g_cs);
}
}

BOOL WINAPI DllMain(HINSTANCE, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_cs);
    return TRUE;
}
