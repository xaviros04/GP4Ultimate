// Wraps the game's IDirectInput8 so device enumeration shows the composite controller
// (first, by default) and hides the physical devices it is built from.
#pragma once
#include "device_manager.h"
#include "log.h"
#include "mod.h"
#include "traits.h"
#include "virtual_device.h"

namespace gp4u {

bool ensure_device_manager();   // dllmain.cpp: lazy DeviceManager::init with settings

template <typename T>
class ProxyDirectInput : public T::DI {
public:
    explicit ProxyDirectInput(typename T::DI* real) : real_(real) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == T::iid_di()) {
            *ppv = static_cast<typename T::DI*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, ppv);
    }
    STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) {
            real_->Release();
            delete this;
        }
        return (ULONG)r;
    }

    STDMETHOD(CreateDevice)(REFGUID rguid, typename T::Dev** out, LPUNKNOWN outer) override {
        if (out && rguid == kVirtualInstance && ensure_device_manager()) {
            *out = new VirtualDevice<T>();
            logf("game: CreateDevice(virtual)");
            return DI_OK;
        }
        return real_->CreateDevice(rguid, out, outer);
    }

    STDMETHOD(EnumDevices)(DWORD type, typename T::EnumDevCb cb, LPVOID ref, DWORD flags) override {
        if (!cb) return DIERR_INVALIDPARAM;
        if (!ensure_device_manager()) return real_->EnumDevices(type, cb, ref, flags);
        DeviceManager& dm = DeviceManager::get();
        bool want_virtual = includes_virtual(type, flags);
        Ctx ctx{cb, ref, false, dm.config().hide_physical, dm.config().hide_others};
        logf("game: EnumDevices(type=0x%lx, flags=0x%lx)%s", (unsigned long)type, (unsigned long)flags,
             want_virtual ? " + virtual" : "");
        if (want_virtual && dm.config().virtual_first && !emit_virtual(ctx)) return DI_OK;
        HRESULT hr = real_->EnumDevices(type, filter_cb, &ctx, flags);
        if (want_virtual && !dm.config().virtual_first && !ctx.stopped) emit_virtual(ctx);
        return hr;
    }

    STDMETHOD(GetDeviceStatus)(REFGUID rguid) override {
        if (rguid == kVirtualInstance) return DI_OK;
        return real_->GetDeviceStatus(rguid);
    }
    STDMETHOD(RunControlPanel)(HWND hwnd, DWORD flags) override { return real_->RunControlPanel(hwnd, flags); }
    STDMETHOD(Initialize)(HINSTANCE h, DWORD v) override { return real_->Initialize(h, v); }
    STDMETHOD(FindDevice)(REFGUID rguid, typename T::Str name, LPGUID out) override {
        return real_->FindDevice(rguid, name, out);
    }
    STDMETHOD(EnumDevicesBySemantics)(typename T::Str user, typename T::ActionFormat* af, typename T::SemCb cb,
                                      LPVOID ref, DWORD flags) override {
        return real_->EnumDevicesBySemantics(user, af, cb, ref, flags);
    }
    STDMETHOD(ConfigureDevices)(LPDICONFIGUREDEVICESCALLBACK cb, typename T::CfgParams* p, DWORD flags,
                                LPVOID ref) override {
        return real_->ConfigureDevices(cb, p, flags, ref);
    }

private:
    struct Ctx {
        typename T::EnumDevCb cb;
        LPVOID ref;
        bool stopped;
        bool hide;
        bool hide_others;
    };

    // Joysticks, wheels, gamepads, flight sticks... (not keyboards, mice or other devices).
    static bool is_game_controller(DWORD devtype) {
        DWORD t = devtype & 0xFF;
        return (t >= DI8DEVTYPE_JOYSTICK && t <= DI8DEVTYPE_1STPERSON) || t == DI8DEVTYPE_SUPPLEMENTAL;
    }

    static bool includes_virtual(DWORD type, DWORD flags) {
        DWORD vt = virtual_devtype() & 0xFF;
        bool type_ok = type == DI8DEVCLASS_ALL || type == DI8DEVCLASS_GAMECTRL || (type & 0xFF) == vt;
        if (!type_ok) return false;
        if ((flags & DIEDFL_FORCEFEEDBACK) && !(DeviceManager::get().ffb_device() && mod().ffb_mode != FfbMode::Off))
            return false;
        return true;
    }

    static bool emit_virtual(Ctx& ctx) {
        typename T::DevInst d;
        VirtualDevice<T>::fill_device_instance(d);
        if (ctx.cb(&d, ctx.ref) == DIENUM_STOP) { ctx.stopped = true; return false; }
        return true;
    }

    static BOOL CALLBACK filter_cb(const typename T::DevInst* d, LPVOID r) {
        Ctx& ctx = *(Ctx*)r;
        if (ctx.hide && DeviceManager::get().is_claimed(d->guidInstance)) return DIENUM_CONTINUE;
        if (ctx.hide_others && is_game_controller(d->dwDevType)) return DIENUM_CONTINUE;
        BOOL res = ctx.cb(d, ctx.ref);
        if (res == DIENUM_STOP) ctx.stopped = true;
        return res;
    }

    virtual ~ProxyDirectInput() = default;
    LONG ref_ = 1;
    typename T::DI* real_;
};

}  // namespace gp4u
