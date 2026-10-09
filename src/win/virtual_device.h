// The composite controller GP4 sees: one IDirectInputDevice8 whose axes, buttons and
// POVs come from several physical devices (DeviceManager), and whose force feedback goes
// to the wheel (pass-through) or is replaced by the telemetry FFB engine.
//
// Implements the parts of the DirectInput device contract games rely on: arbitrary data
// formats (c_dfDIJoystick, c_dfDIJoystick2 or custom), immediate and buffered data,
// object enumeration/info by offset/id/usage, per-axis range/deadzone/saturation.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "device_manager.h"
#include "effect_proxy.h"
#include "ffb_engine.h"
#include "log.h"
#include "mod.h"
#include "traits.h"

namespace gp4u {

inline void convert_effect_info(const DIEFFECTINFOA& a, DIEFFECTINFOA& o) { o = a; o.dwSize = sizeof o; }
inline void convert_effect_info(const DIEFFECTINFOA& a, DIEFFECTINFOW& o) {
    std::memset(&o, 0, sizeof o);
    o.dwSize = sizeof o;
    o.guid = a.guid;
    o.dwEffType = a.dwEffType;
    o.dwStaticParams = a.dwStaticParams;
    o.dwDynamicParams = a.dwDynamicParams;
    set_name(o.tszName, MAX_PATH, a.tszName);
}

DWORD virtual_devtype();

inline BOOL CALLBACK collect_effect_info(LPCDIEFFECTINFOA e, LPVOID ref) {
    ((std::vector<DIEFFECTINFOA>*)ref)->push_back(*e);
    return DIENUM_CONTINUE;
}

template <typename T>
class VirtualDevice : public T::Dev {
    using ObjInst = typename T::ObjInst;
    using DevInst = typename T::DevInst;

    struct VObj {
        enum Kind { Axis, Button, Pov } kind;
        int idx;                // axis id / button index / pov index
        DWORD id;               // DIDFT type | instance (| FFACTUATOR)
        DWORD default_ofs;      // offset in DIJOYSTATE2
        DWORD bound_ofs = ~0u;  // offset in the game's data format, if bound
        LONG lmin = 0, lmax = 65535;
        DWORD deadzone = 0, saturation = 10000;
        bool ff = false;
        const GUID* guid;
        WORD usage_page, usage;
        std::string name;
    };
    struct FmtEntry {
        DWORD ofs;
        int obj;    // index into objs_ or -1
        int kind;   // VObj::Kind of the entry type (for unbound POV fill)
    };

public:
    VirtualDevice() {
        DeviceManager& dm = DeviceManager::get();
        const MappingConfig& m = dm.config();
        has_ffb_ = dm.ffb_device() != nullptr && mod().ffb_mode != FfbMode::Off;
        static const WORD axis_usage[8] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x36};
        static const char* axis_label[8] = {"X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation", "Slider", "Slider"};
        int ninst = 0;
        for (int a = 0; a < AX_COUNT; ++a) {
            bool bound = false;
            for (auto& b : m.axes) bound |= b.target == a;
            if (!bound) continue;
            VObj o{};
            o.kind = VObj::Axis;
            o.idx = a;
            o.ff = has_ffb_ && a == AX_X;
            o.id = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(ninst++) | (o.ff ? DIDFT_FFACTUATOR : 0);
            o.default_ofs = joystate2_axis_offset(a);
            o.guid = &axis_guid(a);
            o.usage_page = 1;
            o.usage = axis_usage[a];
            o.name = axis_label[a];
            objs_.push_back(o);
        }
        for (int i = 0; i < dm.num_buttons(); ++i) {
            VObj o{};
            o.kind = VObj::Button;
            o.idx = i;
            o.id = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i);
            o.default_ofs = (DWORD)(FIELD_OFFSET(DIJOYSTATE2, rgbButtons) + i);
            o.guid = &GUID_Button;
            o.usage_page = 9;
            o.usage = (WORD)(i + 1);
            o.name = "Button " + std::to_string(i);
            objs_.push_back(o);
        }
        for (int i = 0; i < dm.num_povs(); ++i) {
            VObj o{};
            o.kind = VObj::Pov;
            o.idx = i;
            o.id = DIDFT_POV | DIDFT_MAKEINSTANCE(i);
            o.default_ofs = (DWORD)(FIELD_OFFSET(DIJOYSTATE2, rgdwPOV) + i * 4);
            o.guid = &GUID_POV;
            o.usage_page = 1;
            o.usage = 0x39;
            o.name = "Hat " + std::to_string(i);
            objs_.push_back(o);
        }
        bind_format(joystate2_format());
        logf("virtual device (%s) created: %u objects, ffb=%s", T::tag, (unsigned)objs_.size(), has_ffb_ ? "yes" : "no");
    }

    // ---------------------------------------------------------------- IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == T::iid_dev()) {
            *ppv = static_cast<typename T::Dev*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) {
            logf("virtual device released");
            if (started_engine_) FfbEngine::get().stop();
            delete this;
        }
        return (ULONG)r;
    }

    // ---------------------------------------------------------------- device
    STDMETHOD(GetCapabilities)(LPDIDEVCAPS caps) override {
        if (!caps || caps->dwSize < sizeof(DIDEVCAPS_DX3)) return DIERR_INVALIDPARAM;
        DWORD size = caps->dwSize;
        DIDEVCAPS c{};
        c.dwSize = size;
        c.dwFlags = DIDC_ATTACHED | DIDC_POLLEDDEVICE | DIDC_POLLEDDATAFORMAT;
        c.dwDevType = virtual_devtype();
        for (auto& o : objs_) {
            if (o.kind == VObj::Axis) ++c.dwAxes;
            else if (o.kind == VObj::Button) ++c.dwButtons;
            else ++c.dwPOVs;
        }
        if (has_ffb_) {
            const DIDEVCAPS& r = DeviceManager::get().ffb_device()->caps;
            c.dwFlags |= r.dwFlags & (DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE | DIDC_SATURATION |
                                      DIDC_POSNEGCOEFFICIENTS | DIDC_POSNEGSATURATION | DIDC_DEADBAND | DIDC_STARTDELAY);
            c.dwFFSamplePeriod = r.dwFFSamplePeriod;
            c.dwFFMinTimeResolution = r.dwFFMinTimeResolution;
            c.dwFirmwareRevision = r.dwFirmwareRevision;
            c.dwHardwareRevision = r.dwHardwareRevision;
            c.dwFFDriverVersion = r.dwFFDriverVersion;
        }
        std::memcpy(caps, &c, std::min<size_t>(size, sizeof c));
        return DI_OK;
    }

    STDMETHOD(EnumObjects)(typename T::EnumObjCb cb, LPVOID ref, DWORD flags) override {
        if (!cb) return DIERR_INVALIDPARAM;
        DWORD types = flags & 0xFF;
        for (auto& o : objs_) {
            if (types) {
                bool ok = (o.kind == VObj::Axis && (types & DIDFT_AXIS)) ||
                          (o.kind == VObj::Button && (types & DIDFT_BUTTON)) ||
                          (o.kind == VObj::Pov && (types & DIDFT_POV));
                if (!ok) continue;
            }
            if ((flags & DIDFT_FFACTUATOR) && !o.ff) continue;
            ObjInst oi;
            fill_obj(o, oi);
            if (cb(&oi, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }

    STDMETHOD(GetProperty)(REFGUID rguid, LPDIPROPHEADER ph) override {
        if (!ph) return DIERR_INVALIDPARAM;
        uintptr_t p = prop_id(rguid);
        VObj* o = ph->dwHow == DIPH_DEVICE ? nullptr : find_obj(ph->dwObj, ph->dwHow);
        if (ph->dwHow != DIPH_DEVICE && !o) return DIERR_OBJECTNOTFOUND;
        VObj* axis = o ? o : first_axis();
        if (p == prop_id(DIPROP_RANGE)) {
            if (!axis || axis->kind != VObj::Axis) return DIERR_UNSUPPORTED;
            auto* r = (DIPROPRANGE*)ph;
            r->lMin = axis->lmin;
            r->lMax = axis->lmax;
            return DI_OK;
        }
        if (p == prop_id(DIPROP_DEADZONE) || p == prop_id(DIPROP_SATURATION)) {
            if (!axis) return DIERR_UNSUPPORTED;
            ((DIPROPDWORD*)ph)->dwData = p == prop_id(DIPROP_DEADZONE) ? axis->deadzone : axis->saturation;
            return DI_OK;
        }
        DIPROPDWORD* d = (DIPROPDWORD*)ph;
        if (p == prop_id(DIPROP_BUFFERSIZE)) { d->dwData = buf_size_; return DI_OK; }
        if (p == prop_id(DIPROP_AXISMODE)) { d->dwData = DIPROPAXISMODE_ABS; return DI_OK; }
        if (p == prop_id(DIPROP_AUTOCENTER)) { d->dwData = autocenter_; return DI_OK; }
        if (p == prop_id(DIPROP_FFGAIN)) { d->dwData = ffgain_; return DI_OK; }
        if (p == prop_id(DIPROP_GRANULARITY)) { d->dwData = 1; return DI_OK; }
        if (p == prop_id(DIPROP_CALIBRATIONMODE)) { d->dwData = calibration_mode_; return DI_OK; }
        if (p == prop_id(DIPROP_VIDPID)) { d->dwData = MAKELONG(0x1209, 0x4750); return DI_OK; }
        if (p == prop_id(DIPROP_FFLOAD)) { d->dwData = 0; return DI_OK; }
        if (p == prop_id(DIPROP_PRODUCTNAME) || p == prop_id(DIPROP_INSTANCENAME)) {
            auto* s = (DIPROPSTRING*)ph;
            set_name(s->wsz, MAX_PATH, DeviceManager::get().config().virtual_name);
            return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }

    STDMETHOD(SetProperty)(REFGUID rguid, LPCDIPROPHEADER ph) override {
        if (!ph) return DIERR_INVALIDPARAM;
        uintptr_t p = prop_id(rguid);
        std::vector<VObj*> targets;
        if (ph->dwHow == DIPH_DEVICE) {
            for (auto& o : objs_)
                if (o.kind == VObj::Axis) targets.push_back(&o);
        } else {
            VObj* o = find_obj(ph->dwObj, ph->dwHow);
            if (!o) return DIERR_OBJECTNOTFOUND;
            targets.push_back(o);
        }
        if (p == prop_id(DIPROP_RANGE)) {
            auto* r = (const DIPROPRANGE*)ph;
            if (r->lMin >= r->lMax) return DIERR_INVALIDPARAM;
            for (VObj* o : targets)
                if (o->kind == VObj::Axis) { o->lmin = r->lMin; o->lmax = r->lMax; }
            logf("game: axis range %ld..%ld (%u axes)", (long)r->lMin, (long)r->lMax, (unsigned)targets.size());
            return DI_OK;
        }
        const DIPROPDWORD* d = (const DIPROPDWORD*)ph;
        if (p == prop_id(DIPROP_DEADZONE) || p == prop_id(DIPROP_SATURATION)) {
            if (d->dwData > 10000) return DIERR_INVALIDPARAM;
            for (VObj* o : targets)
                if (o->kind == VObj::Axis) (p == prop_id(DIPROP_DEADZONE) ? o->deadzone : o->saturation) = d->dwData;
            return DI_OK;
        }
        if (p == prop_id(DIPROP_BUFFERSIZE)) {
            buf_size_ = d->dwData;
            while (buf_.size() > buf_size_) buf_.pop_front();
            return DI_OK;
        }
        if (p == prop_id(DIPROP_AUTOCENTER) || p == prop_id(DIPROP_FFGAIN)) {
            (p == prop_id(DIPROP_AUTOCENTER) ? autocenter_ : ffgain_) = d->dwData;
            // Only the game's own FF (pass-through) should change the wheel's settings.
            if (has_ffb_ && mod().ffb_mode == FfbMode::Game) {
                std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
                DeviceManager::get().ffb_device()->dev->SetProperty(rguid, ph);
            }
            return DI_OK;
        }
        if (p == prop_id(DIPROP_CALIBRATIONMODE)) {
            // GP4 sets RAW and drops the device if this fails (GP4.exe 0x5061ea), then
            // calibrates axes in its own menu. Our values are already conditioned, so both
            // modes read the same; we just report back what was set.
            calibration_mode_ = d->dwData;
            return DI_OK;
        }
        return DI_OK;  // axis mode, ... accepted and ignored
    }

    STDMETHOD(Acquire)() override {
        bool was = acquired_;
        acquired_ = true;
        DeviceManager::get().acquire();
        return was ? S_FALSE : DI_OK;
    }
    STDMETHOD(Unacquire)() override {
        if (!acquired_) return DI_NOEFFECT;
        acquired_ = false;
        DeviceManager::get().unacquire();
        return DI_OK;
    }

    STDMETHOD(GetDeviceState)(DWORD cb, LPVOID data) override {
        if (!data) return DIERR_INVALIDPARAM;
        if (!acquired_) return DIERR_NOTACQUIRED;
        if (cb != data_size_) return DIERR_INVALIDPARAM;
        refresh();
        uint8_t* out = (uint8_t*)data;
        std::memset(out, 0, cb);
        for (size_t i = 0; i < fmt_.size(); ++i) {
            const FmtEntry& e = fmt_[i];
            DWORD v = cur_[i];
            if (e.kind == VObj::Button) {
                if (e.ofs < cb) out[e.ofs] = (uint8_t)v;
            } else if (e.ofs + 4 <= cb) {
                std::memcpy(out + e.ofs, &v, 4);
            }
        }
        return DI_OK;
    }

    STDMETHOD(GetDeviceData)(DWORD cbObj, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inout, DWORD flags) override {
        if (!inout) return DIERR_INVALIDPARAM;
        if (!acquired_) return DIERR_NOTACQUIRED;
        if (buf_size_ == 0) return DIERR_NOTBUFFERED;
        if (rgdod && cbObj < sizeof(DIDEVICEOBJECTDATA_DX3)) return DIERR_INVALIDPARAM;
        refresh();
        bool peek = (flags & DIGDD_PEEK) != 0;
        DWORD n = std::min<DWORD>(*inout, (DWORD)buf_.size());
        if (rgdod) {
            uint8_t* dst = (uint8_t*)rgdod;
            for (DWORD i = 0; i < n; ++i)
                std::memcpy(dst + (size_t)i * cbObj, &buf_[i], std::min<size_t>(cbObj, sizeof(DIDEVICEOBJECTDATA)));
        }
        if (!peek) buf_.erase(buf_.begin(), buf_.begin() + n);
        *inout = n;
        HRESULT hr = overflow_ ? DI_BUFFEROVERFLOW : DI_OK;
        if (!peek) overflow_ = false;
        return hr;
    }

    STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT df) override {
        if (!df || !df->rgodf || df->dwObjSize < sizeof(DIOBJECTDATAFORMAT)) return DIERR_INVALIDPARAM;
        if (acquired_) return DIERR_ACQUIRED;
        bind_format(df);
        return DI_OK;
    }

    STDMETHOD(SetEventNotification)(HANDLE h) override { event_ = h; return DI_OK; }

    STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD flags) override {
        logf("game: SetCooperativeLevel(hwnd=%p, flags=0x%lx)", (void*)hwnd, (unsigned long)flags);
        DeviceManager::get().set_cooperative(hwnd);
        FfbMode m = mod().ffb_mode;
        if (has_ffb_ && (m == FfbMode::Telemetry || m == FfbMode::Mix) && !started_engine_) {
            started_engine_ = FfbEngine::get().start();
        }
        return DI_OK;
    }

    STDMETHOD(GetObjectInfo)(ObjInst* oi, DWORD obj, DWORD how) override {
        if (!oi || oi->dwSize == 0) return DIERR_INVALIDPARAM;
        VObj* o = find_obj(obj, how);
        if (!o) return DIERR_OBJECTNOTFOUND;
        DWORD size = std::min<DWORD>(oi->dwSize, sizeof(ObjInst));
        ObjInst full;
        fill_obj(*o, full);
        full.dwSize = oi->dwSize;
        std::memcpy(oi, &full, size);
        return DI_OK;
    }

    STDMETHOD(GetDeviceInfo)(DevInst* di) override {
        if (!di || di->dwSize == 0) return DIERR_INVALIDPARAM;
        DWORD size = std::min<DWORD>(di->dwSize, sizeof(DevInst));
        DevInst full;
        fill_device_instance(full);
        full.dwSize = di->dwSize;
        std::memcpy(di, &full, size);
        return DI_OK;
    }

    STDMETHOD(RunControlPanel)(HWND, DWORD) override { return DI_OK; }
    STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }

    // ---------------------------------------------------------------- force feedback
    STDMETHOD(CreateEffect)(REFGUID rguid, LPCDIEFFECT eff, LPDIRECTINPUTEFFECT* out, LPUNKNOWN) override {
        if (!out) return DIERR_INVALIDPARAM;
        *out = nullptr;
        if (!has_ffb_) return DIERR_UNSUPPORTED;
        FfbMode m = mod().ffb_mode;
        DeviceManager& dm = DeviceManager::get();
        DWORD axis_ofs = dm.virtual_to_real_axis_offset(AX_X);
        double gain = mod().game_effects_gain;
        if (m == FfbMode::Game || m == FfbMode::Mix) {
            IDirectInputEffect* real = nullptr;
            HRESULT hr;
            {
                std::lock_guard<std::mutex> lk(dm.mutex());
                dm.ffb_ready_locked();
                if (eff) {
                    std::vector<DWORD> axes;
                    std::vector<LONG> dirs;
                    DIEFFECT t = EffectProxy::translate(*eff, axis_ofs, gain, axes, dirs);
                    hr = dm.ffb_device()->dev->CreateEffect(rguid, &t, &real, nullptr);
                } else {
                    hr = dm.ffb_device()->dev->CreateEffect(rguid, nullptr, &real, nullptr);
                }
            }
            logf("game: CreateEffect %s -> 0x%08lx", guid_str(&rguid).c_str(), (unsigned long)hr);
            if (FAILED(hr) || !real) return hr;
            *out = new EffectProxy(rguid, real, axis_ofs, gain);
            return hr;
        }
        logf("game: CreateEffect %s -> null effect (mode %s)", guid_str(&rguid).c_str(), ffb_mode_name(m));
        *out = new EffectProxy(rguid, nullptr, axis_ofs, gain);
        return DI_OK;
    }

    STDMETHOD(EnumEffects)(typename T::EnumEffCb cb, LPVOID ref, DWORD type) override {
        if (!cb) return DIERR_INVALIDPARAM;
        if (!has_ffb_) return DI_OK;
        std::vector<DIEFFECTINFOA> list;
        {
            std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
            DeviceManager::get().ffb_device()->dev->EnumEffects(collect_effect_info, &list, type);
        }
        // Call the game outside our lock: it may create effects from the callback.
        for (auto& e : list) {
            typename T::EffInfo info;
            convert_effect_info(e, info);
            if (cb(&info, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }

    STDMETHOD(GetEffectInfo)(typename T::EffInfo* info, REFGUID rguid) override {
        if (!info) return DIERR_INVALIDPARAM;
        if (!has_ffb_) return DIERR_DEVICENOTREG;
        DIEFFECTINFOA a{};
        a.dwSize = sizeof a;
        HRESULT hr;
        {
            std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
            hr = DeviceManager::get().ffb_device()->dev->GetEffectInfo(&a, rguid);
        }
        if (SUCCEEDED(hr)) convert_effect_info(a, *info);
        return hr;
    }

    STDMETHOD(GetForceFeedbackState)(LPDWORD out) override {
        if (!out) return DIERR_INVALIDPARAM;
        FfbMode m = mod().ffb_mode;
        if (has_ffb_ && (m == FfbMode::Game || m == FfbMode::Mix)) {
            std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
            return DeviceManager::get().ffb_device()->dev->GetForceFeedbackState(out);
        }
        *out = DIGFFS_POWERON | DIGFFS_ACTUATORSON | DIGFFS_EMPTY;
        return DI_OK;
    }

    STDMETHOD(SendForceFeedbackCommand)(DWORD cmd) override {
        FfbMode m = mod().ffb_mode;
        if (!has_ffb_ || (m != FfbMode::Game && m != FfbMode::Mix)) return DI_OK;
        HRESULT hr;
        {
            std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
            hr = DeviceManager::get().ffb_device()->dev->SendForceFeedbackCommand(cmd);
        }
        if (cmd & (DISFFC_RESET | DISFFC_STOPALL)) FfbEngine::get().on_device_reset();
        return hr;
    }

    STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
    STDMETHOD(Escape)(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }

    STDMETHOD(Poll)() override {
        if (!acquired_) return DIERR_NOTACQUIRED;
        refresh();
        return DI_OK;
    }

    STDMETHOD(SendDeviceData)(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD inout, DWORD) override {
        if (inout) *inout = 0;
        return DI_OK;
    }
    STDMETHOD(EnumEffectsInFile)(typename T::Str, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(WriteEffectToFile)(typename T::Str, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(BuildActionMap)(typename T::ActionFormat*, typename T::Str, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(SetActionMap)(typename T::ActionFormat*, typename T::Str, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(GetImageInfo)(typename T::ImageHdr*) override { return DIERR_UNSUPPORTED; }

    static void fill_device_instance(DevInst& d) {
        std::memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        d.guidInstance = kVirtualInstance;
        d.guidProduct = kVirtualProduct;
        d.dwDevType = virtual_devtype();
        const std::string& n = DeviceManager::get().config().virtual_name;
        set_name(d.tszInstanceName, MAX_PATH, n);
        set_name(d.tszProductName, MAX_PATH, n);
        if (PhysDevice* f = DeviceManager::get().ffb_device()) {
            // guidFFDriver of the wheel; read once from its own device info.
            DIDEVICEINSTANCEA a{};
            a.dwSize = sizeof a;
            std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
            if (SUCCEEDED(f->dev->GetDeviceInfo(&a))) d.guidFFDriver = a.guidFFDriver;
        }
        d.wUsagePage = 1;
        d.wUsage = (d.dwDevType & 0xFF) == DI8DEVTYPE_GAMEPAD ? 5 : 4;
    }

private:
    virtual ~VirtualDevice() = default;

    VObj* first_axis() {
        for (auto& o : objs_)
            if (o.kind == VObj::Axis) return &o;
        return nullptr;
    }

    VObj* find_obj(DWORD obj, DWORD how) {
        for (auto& o : objs_) {
            switch (how) {
                case DIPH_BYOFFSET:
                    if (o.bound_ofs == obj) return &o;
                    break;
                case DIPH_BYID:
                    if (DIDFT_GETINSTANCE(o.id) == DIDFT_GETINSTANCE(obj) && (DIDFT_GETTYPE(o.id) & DIDFT_GETTYPE(obj)))
                        return &o;
                    break;
                case DIPH_BYUSAGE:
                    if (obj == (DWORD)MAKELONG(o.usage, o.usage_page)) return &o;
                    break;
            }
        }
        return nullptr;
    }

    void fill_obj(const VObj& o, ObjInst& oi) const {
        std::memset(&oi, 0, sizeof oi);
        oi.dwSize = sizeof oi;
        oi.guidType = *o.guid;
        oi.dwOfs = o.bound_ofs != ~0u ? o.bound_ofs : o.default_ofs;
        oi.dwType = o.id;
        oi.dwFlags = (o.kind == VObj::Axis ? DIDOI_ASPECTPOSITION : 0) | (o.ff ? DIDOI_FFACTUATOR : 0);
        set_name(oi.tszName, MAX_PATH, o.name);
        if (o.ff) { oi.dwFFMaxForce = 10000; oi.dwFFForceResolution = 1; }
        oi.wUsagePage = o.usage_page;
        oi.wUsage = o.usage;
    }

    // Binds each entry of the game's data format to one of our objects, following
    // DirectInput's rules: type must match, GUID if given, instance unless ANYINSTANCE,
    // first free object wins. Velocity/acceleration/force aspects stay unbound.
    void bind_format(const DIDATAFORMAT* df) {
        for (auto& o : objs_) o.bound_ofs = ~0u;
        fmt_.clear();
        std::vector<bool> used(objs_.size(), false);
        const uint8_t* base = (const uint8_t*)df->rgodf;
        for (DWORD i = 0; i < df->dwNumObjs; ++i) {
            const DIOBJECTDATAFORMAT& f = *(const DIOBJECTDATAFORMAT*)(base + (size_t)i * df->dwObjSize);
            DWORD t = DIDFT_GETTYPE(f.dwType);
            int kind = (t & DIDFT_AXIS) ? VObj::Axis : (t & DIDFT_BUTTON) ? VObj::Button : (t & DIDFT_POV) ? VObj::Pov : -1;
            if (kind < 0) continue;
            DWORD aspect = f.dwFlags & DIDOI_ASPECTMASK;
            bool wrong_aspect = kind == VObj::Axis && aspect != 0 && aspect != DIDOI_ASPECTPOSITION;
            DWORD inst = DIDFT_GETINSTANCE(f.dwType);
            bool any = inst == 0xFFFF;
            int match = -1;
            if (!wrong_aspect) {
                for (size_t k = 0; k < objs_.size(); ++k) {
                    const VObj& o = objs_[k];
                    if (used[k] || o.kind != kind) continue;
                    if (f.pguid && *f.pguid != *o.guid) continue;
                    if (!any && DIDFT_GETINSTANCE(o.id) != inst) continue;
                    match = (int)k;
                    break;
                }
            }
            if (match >= 0) {
                used[match] = true;
                objs_[match].bound_ofs = f.dwOfs;
            }
            fmt_.push_back({f.dwOfs, match, kind});
        }
        data_size_ = df->dwDataSize;
        cur_.assign(fmt_.size(), 0);
        for (size_t i = 0; i < fmt_.size(); ++i)
            if (fmt_[i].kind == VObj::Pov) cur_[i] = 0xFFFFFFFF;
        buf_.clear();
        unsigned bound = 0;
        for (auto& e : fmt_) bound += e.obj >= 0;
        logf("game: SetDataFormat size=%lu objects=%lu bound=%u", (unsigned long)df->dwDataSize,
             (unsigned long)df->dwNumObjs, bound);
    }

    static DWORD encode_axis(const VObj& o, double v01) {
        double v = v01;
        if (o.deadzone > 0 || o.saturation < 10000) {
            double c = 2.0 * v - 1.0, m = std::fabs(c);
            double dz = o.deadzone / 10000.0, sat = std::max(dz + 1e-6, o.saturation / 10000.0);
            m = m <= dz ? 0.0 : (m >= sat ? 1.0 : (m - dz) / (sat - dz));
            v = 0.5 + 0.5 * (c < 0 ? -m : m);
        }
        double r = o.lmin + v * ((double)o.lmax - (double)o.lmin);
        return (DWORD)(LONG)std::lround(r);
    }

    void refresh() {
        VirtualState vs = DeviceManager::get().poll();
        bool changed = false;
        DWORD now = GetTickCount();
        for (size_t i = 0; i < fmt_.size(); ++i) {
            const FmtEntry& e = fmt_[i];
            if (e.obj < 0) continue;
            const VObj& o = objs_[e.obj];
            DWORD v;
            if (o.kind == VObj::Axis) v = encode_axis(o, vs.axis[o.idx]);
            else if (o.kind == VObj::Button) v = vs.buttons[o.idx];
            else v = vs.pov[o.idx];
            if (v != cur_[i]) {
                cur_[i] = v;
                changed = true;
                if (buf_size_) {
                    if (buf_.size() >= buf_size_) { buf_.pop_front(); overflow_ = true; }
                    DIDEVICEOBJECTDATA ev{};
                    ev.dwOfs = e.ofs;
                    ev.dwData = v;
                    ev.dwTimeStamp = now;
                    ev.dwSequence = ++seq_;
                    buf_.push_back(ev);
                }
            }
        }
        if (changed && event_) SetEvent(event_);
    }

    LONG ref_ = 1;
    bool acquired_ = false;
    bool has_ffb_ = false;
    bool started_engine_ = false;
    std::vector<VObj> objs_;
    std::vector<FmtEntry> fmt_;
    std::vector<DWORD> cur_;
    DWORD data_size_ = 0;
    DWORD buf_size_ = 0;
    std::deque<DIDEVICEOBJECTDATA> buf_;
    bool overflow_ = false;
    DWORD seq_ = 0;
    HANDLE event_ = nullptr;
    DWORD autocenter_ = DIPROPAUTOCENTER_ON;
    DWORD ffgain_ = 10000;
    DWORD calibration_mode_ = DIPROPCALIBRATIONMODE_COOKED;
};

}  // namespace gp4u
