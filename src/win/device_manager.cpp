#include "device_manager.h"

#include <algorithm>
#include <cstring>

#include "log.h"

namespace gp4u {

DeviceManager& DeviceManager::get() {
    static DeviceManager m;
    return m;
}


struct EnumCtx {
    std::vector<DIDEVICEINSTANCEA> list;
};

static BOOL CALLBACK enum_cb(LPCDIDEVICEINSTANCEA inst, LPVOID ref) {
    ((EnumCtx*)ref)->list.push_back(*inst);
    return DIENUM_CONTINUE;
}

static bool matches(const DeviceSelector& sel, const DIDEVICEINSTANCEA& d, int index) {
    // DirectInput product GUIDs of HID devices are {PPPPVVVV-0000-0000-0000-504944564944}.
    uint16_t vid = (uint16_t)(d.guidProduct.Data1 & 0xFFFF), pid = (uint16_t)(d.guidProduct.Data1 >> 16);
    return selector_matches(sel, d.tszProductName, d.tszInstanceName, vid, pid, guid_str(&d.guidInstance), index);
}

struct ObjCtx {
    PhysDevice* d;
};

static BOOL CALLBACK obj_cb(LPCDIDEVICEOBJECTINSTANCEA o, LPVOID ref) {
    PhysDevice* d = ((ObjCtx*)ref)->d;
    for (int a = 0; a < AX_COUNT; ++a) {
        if (a < 6 && o->guidType == axis_guid(a)) d->axis_present[a] = true;
    }
    if (o->guidType == GUID_Slider) {
        if (!d->axis_present[AX_S0]) d->axis_present[AX_S0] = true;
        else d->axis_present[AX_S1] = true;
    }
    return DIENUM_CONTINUE;
}

bool DeviceManager::open(PhysDevice& d, const DIDEVICEINSTANCEA& inst) {
    d.instance = inst.guidInstance;
    d.product = inst.guidProduct;
    d.name = inst.tszProductName;
    HRESULT hr = di_->CreateDevice(inst.guidInstance, &d.dev, nullptr);
    if (FAILED(hr) || !d.dev) { logf("  %s: CreateDevice failed 0x%08lx", d.alias.c_str(), (unsigned long)hr); return false; }
    hr = d.dev->SetDataFormat(joystate2_format());
    if (FAILED(hr)) { logf("  %s: SetDataFormat failed 0x%08lx", d.alias.c_str(), (unsigned long)hr); return false; }
    // Normalise every axis to 0..65535, no deadzone/saturation: conditioning is ours.
    DIPROPRANGE r{};
    r.diph.dwSize = sizeof r; r.diph.dwHeaderSize = sizeof r.diph; r.diph.dwHow = DIPH_DEVICE;
    r.lMin = 0; r.lMax = 65535;
    d.dev->SetProperty(DIPROP_RANGE, &r.diph);
    DIPROPDWORD p{};
    p.diph.dwSize = sizeof p; p.diph.dwHeaderSize = sizeof p.diph; p.diph.dwHow = DIPH_DEVICE;
    p.dwData = 0;
    d.dev->SetProperty(DIPROP_DEADZONE, &p.diph);
    p.dwData = 10000;
    d.dev->SetProperty(DIPROP_SATURATION, &p.diph);
    d.caps.dwSize = sizeof d.caps;
    d.dev->GetCapabilities(&d.caps);
    d.ffb = (d.caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
    ObjCtx ctx{&d};
    d.dev->EnumObjects(obj_cb, &ctx, DIDFT_AXIS);
    std::string axes;
    for (int a = 0; a < AX_COUNT; ++a)
        if (d.axis_present[a]) { axes += axis_name(a); axes += ' '; }
    logf("  %s -> \"%s\" %s  axes: %sbuttons: %lu povs: %lu%s", d.alias.c_str(), d.name.c_str(),
         guid_str(&d.instance).c_str(), axes.c_str(), (unsigned long)d.caps.dwButtons,
         (unsigned long)d.caps.dwPOVs, d.ffb ? "  [force feedback]" : "");
    return true;
}

bool DeviceManager::init(DI8CreateFn create, const MappingConfig& cfg, bool ffb_background) {
    // Re-entry from inside the set-up (a wheel driver calling back into this DLL while we
    // open its device) must not wait for the lock this thread already holds.
    static thread_local bool in_init = false;
    if (in_init) return false;
    struct Guard { bool& f; Guard(bool& x) : f(x) { f = true; } ~Guard() { f = false; } } guard(in_init);
    std::lock_guard<std::mutex> lk(mtx_);
    if (tried_) return active_;
    tried_ = true;
    cfg_ = cfg;
    ffb_background_ = ffb_background;
    if (!cfg_.enabled()) { logf("input: no [devices]/[axes] configured - pass-through"); return false; }
    HRESULT hr = create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (LPVOID*)&di_, nullptr);
    if (FAILED(hr) || !di_) { logf("input: own DirectInput8Create failed 0x%08lx", (unsigned long)hr); return false; }

    EnumCtx ctx;
    di_->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_cb, &ctx, DIEDFL_ATTACHEDONLY);
    logf("input: %u game controllers attached", (unsigned)ctx.list.size());
    for (size_t i = 0; i < ctx.list.size(); ++i)
        logf("  [%u] \"%s\" vid:pid %04lX:%04lX %s", (unsigned)i, ctx.list[i].tszProductName,
             (unsigned long)(ctx.list[i].guidProduct.Data1 & 0xFFFF), (unsigned long)(ctx.list[i].guidProduct.Data1 >> 16),
             guid_str(&ctx.list[i].guidInstance).c_str());

    devs_.reserve(cfg_.devices.size());
    std::vector<bool> taken(ctx.list.size(), false);
    for (auto& nd : cfg_.devices) {
        PhysDevice d;
        d.alias = nd.alias;
        int found = -1;
        for (size_t i = 0; i < ctx.list.size(); ++i)
            if (!taken[i] && matches(nd.sel, ctx.list[i], (int)i)) { found = (int)i; break; }
        if (found < 0) { logf("input: [devices] %s: no attached controller matches", nd.alias.c_str()); devs_.push_back(d); continue; }
        taken[found] = true;
        if (!open(d, ctx.list[found]) && d.dev) { d.dev->Release(); d.dev = nullptr; }
        devs_.push_back(d);
    }

    int opened = 0;
    for (auto& d : devs_) opened += d.dev != nullptr;
    if (opened == 0) {
        // Nothing of the mapping is connected (or the ini is still the example): presenting
        // an empty virtual controller would only hide the real ones. Stay out of the way.
        logf("input: none of the [devices] is connected - composite device OFF, pass-through");
        return false;
    }

    // Button and POV routing tables.
    for (auto& b : btn_map_) b = BtnSrc{};
    for (auto& p : pov_map_) p = PovSrc{};
    auto index_of = [&](const std::string& alias) {
        for (size_t i = 0; i < devs_.size(); ++i)
            if (devs_[i].alias == alias) return (int)i;
        return -1;
    };
    bool explicit_btn[128] = {};
    for (auto& b : cfg_.buttons) explicit_btn[b.target] = true;
    int next = 0;
    for (auto& alias : cfg_.auto_buttons) {
        int di = index_of(alias);
        int n = (di >= 0 && devs_[di].dev) ? (int)devs_[di].caps.dwButtons : 0;
        for (int i = 0; i < n; ++i) {
            while (next < 128 && explicit_btn[next]) ++next;
            if (next >= 128) break;
            btn_map_[next++] = {di, i};
        }
    }
    for (auto& b : cfg_.buttons) btn_map_[b.target] = {index_of(b.device), b.button};
    num_buttons_ = 0;
    for (int i = 0; i < 128; ++i)
        if (btn_map_[i].dev >= 0) num_buttons_ = i + 1;
    num_povs_ = 0;
    for (auto& p : cfg_.povs) {
        pov_map_[p.target] = {index_of(p.device), p.pov};
        num_povs_ = std::max(num_povs_, p.target + 1);
    }

    // Force feedback target and the axis it acts on (the one feeding virtual X).
    if (!cfg_.ffb_device.empty()) {
        PhysDevice* f = find(cfg_.ffb_device);
        if (f && f->dev && f->ffb) {
            ffb_ = f;
            for (auto& ab : cfg_.axes)
                if (ab.target == AX_X && ab.kind == AxisBinding::Single && ab.a.device == f->alias) {
                    ffb_axis_ = ab.a.axis;
                    ffb_axis_invert_ = ab.a.invert;
                }
            logf("ffb: device %s, axis %s", f->alias.c_str(), axis_name(ffb_axis_));
        } else {
            logf("ffb: device '%s' missing or without force feedback", cfg_.ffb_device.c_str());
        }
    }
    if (num_buttons_ > 8)
        logf("input: WARNING %d virtual buttons, but GP4 only reads buttons 0-7 (GP4.exe 0x5065c2);"
             " map the ones you want explicitly in [buttons] 0..7", num_buttons_);
    active_ = true;
    logf("input: composite device active: %u axes bound, %d buttons, %d POVs", (unsigned)cfg_.axes.size(),
         num_buttons_, num_povs_);
    return true;
}

PhysDevice* DeviceManager::find(const std::string& alias) {
    for (auto& d : devs_)
        if (d.alias == alias) return &d;
    return nullptr;
}

bool DeviceManager::is_claimed(const GUID& instance) const {
    for (auto& d : devs_)
        if (d.dev && d.instance == instance) return true;
    return false;
}

void DeviceManager::apply_coop_locked(PhysDevice& d) {
    if (!d.dev) return;
    d.dev->Unacquire();
    d.acquired = false;
    DWORD flags;
    HWND w = hwnd_;
    if (&d == ffb_) {
        // Force feedback requires exclusive access, which requires a window.
        flags = DISCL_EXCLUSIVE | (ffb_background_ ? DISCL_BACKGROUND : DISCL_FOREGROUND);
    } else {
        flags = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    }
    HRESULT hr = d.dev->SetCooperativeLevel(w, flags);
    if (FAILED(hr)) {
        logf("  %s: SetCooperativeLevel(0x%lx) failed 0x%08lx, falling back to non-exclusive", d.alias.c_str(),
             (unsigned long)flags, (unsigned long)hr);
        d.dev->SetCooperativeLevel(w, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    }
    if (&d == ffb_) {
        // Turn the wheel's own spring off: our engine (or the game) is in charge.
        DIPROPDWORD p{};
        p.diph.dwSize = sizeof p; p.diph.dwHeaderSize = sizeof p.diph; p.diph.dwHow = DIPH_DEVICE;
        p.dwData = DIPROPAUTOCENTER_OFF;
        d.dev->SetProperty(DIPROP_AUTOCENTER, &p.diph);
    }
}

void DeviceManager::set_cooperative(HWND hwnd) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!active_) return;
    hwnd_ = hwnd;
    for (auto& d : devs_) apply_coop_locked(d);
    if (want_acquired_)
        for (auto& d : devs_)
            if (d.dev) acquire_locked(d);
}

bool DeviceManager::acquire_locked(PhysDevice& d) {
    bool ok = SUCCEEDED(d.dev->Acquire());
    if (ok && !d.acquired && &d == ffb_) ++ffb_gen_;
    d.acquired = ok;
    return ok;
}

void DeviceManager::acquire() {
    std::lock_guard<std::mutex> lk(mtx_);
    want_acquired_ = true;
    for (auto& d : devs_)
        if (d.dev && !d.acquired) acquire_locked(d);
}

void DeviceManager::unacquire() {
    std::lock_guard<std::mutex> lk(mtx_);
    want_acquired_ = false;
    for (auto& d : devs_)
        if (d.dev && &d != ffb_) { d.dev->Unacquire(); d.acquired = false; }
    // The FFB wheel stays acquired so the engine keeps control while the game re-acquires.
}

void DeviceManager::read_locked(PhysDevice& d) {
    if (!d.dev) return;
    if (!d.acquired) acquire_locked(d);
    d.dev->Poll();
    HRESULT hr = d.dev->GetDeviceState(sizeof(DIJOYSTATE2), &d.state);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        d.acquired = false;   // DirectInput dropped it (focus change): re-acquiring resets an FF wheel
        if (acquire_locked(d)) {
            d.dev->Poll();
            hr = d.dev->GetDeviceState(sizeof(DIJOYSTATE2), &d.state);
        }
    }
    if (SUCCEEDED(hr)) d.have_state = true;
}

double DeviceManager::raw01(const PhysDevice& d, int axis) const {
    if (!d.have_state || !d.axis_present[axis]) return axis == AX_X ? 0.5 : 0.0;
    LONG v;
    switch (axis) {
        case AX_X: v = d.state.lX; break;
        case AX_Y: v = d.state.lY; break;
        case AX_Z: v = d.state.lZ; break;
        case AX_RX: v = d.state.lRx; break;
        case AX_RY: v = d.state.lRy; break;
        case AX_RZ: v = d.state.lRz; break;
        case AX_S0: v = d.state.rglSlider[0]; break;
        default: v = d.state.rglSlider[1]; break;
    }
    return std::clamp((double)v / 65535.0, 0.0, 1.0);
}

VirtualState DeviceManager::poll() {
    std::lock_guard<std::mutex> lk(mtx_);
    VirtualState vs;
    if (!active_) return vs;
    for (auto& d : devs_) read_locked(d);
    for (auto& b : cfg_.axes) {
        const PhysDevice* da = nullptr;
        const PhysDevice* db = nullptr;
        for (auto& d : devs_) {
            if (d.alias == b.a.device) da = &d;
            if (b.kind == AxisBinding::Combine && d.alias == b.b.device) db = &d;
        }
        double ra = da ? raw01(*da, b.a.axis) : 0.0;
        double rb = db ? raw01(*db, b.b.axis) : 0.0;
        vs.axis[b.target] = apply_binding(b, ra, rb);
        vs.axis_bound[b.target] = true;
    }
    for (int i = 0; i < num_buttons_; ++i) {
        const BtnSrc& s = btn_map_[i];
        if (s.dev < 0 || !devs_[s.dev].have_state || s.btn >= 128) continue;
        vs.buttons[i] = devs_[s.dev].state.rgbButtons[s.btn] & 0x80;
    }
    for (int i = 0; i < num_povs_; ++i) {
        const PovSrc& s = pov_map_[i];
        if (s.dev < 0 || !devs_[s.dev].have_state) continue;
        vs.pov[i] = devs_[s.dev].state.rgdwPOV[s.pov];
    }
    return vs;
}

bool DeviceManager::ffb_ready_locked() {
    if (!ffb_ || !ffb_->dev || !hwnd_) return false;
    if (!ffb_->acquired) acquire_locked(*ffb_);
    return ffb_->acquired;
}

double DeviceManager::steer_locked() {
    if (!ffb_) return 0.0;
    read_locked(*ffb_);
    double v = 2.0 * raw01(*ffb_, ffb_axis_) - 1.0;
    return ffb_axis_invert_ ? -v : v;
}

DWORD DeviceManager::virtual_to_real_axis_offset(int virtual_axis) const {
    for (auto& b : cfg_.axes)
        if (b.target == virtual_axis && b.kind == AxisBinding::Single && ffb_ && b.a.device == ffb_->alias)
            return joystate2_axis_offset(b.a.axis);
    return joystate2_axis_offset(ffb_axis_);
}

}  // namespace gp4u
